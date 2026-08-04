//===- ActorPlan.cpp - FR-62 actor decomposition planning -----------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements `ActorPlan.h`: parse the units' item-graph texts into a
/// function/global access index, pin overridden globals, seed the
/// remaining universe with E2's co-access union-find, condense with the
/// SCC and cross-actor-writer rules until every conflict is resolved, and
/// assign each defined function its role. The graph shape follows the
/// Partition.cpp precedent: plain indices, sorted containers, explicit
/// iteration order — no hash-table iteration order reaches any output.
//
//===----------------------------------------------------------------------===//

#include "ActorPlan.h"

#include "EmitRust/Project/ItemGraph.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <set>

using namespace emitrustcc;

namespace {

/// The hosted output-STREAM sinks whose `Calls` edge counts as a write of
/// the "@stdout" pseudo-global. Deliberately EXCLUDES sprintf/snprintf,
/// which the item graph makes visible as sink nodes too (slice 1) but
/// whose importer lowering (emitSprintf) writes the caller's buffer with
/// no output effect — see the header's doc block.
bool isStdoutSinkName(llvm::StringRef symbol) {
  return symbol == "printf" || symbol == "puts" || symbol == "putchar" ||
         symbol == "fprintf" || symbol == "fwrite";
}

/// The distinguished pseudo-global; planner-side only, never a graph node.
constexpr llvm::StringLiteral kStdout = "@stdout";

/// Union-find with path compression; the root with the SMALLER id
/// survives, so over an index space built from a sorted vector the
/// lexicographically smallest member/name always wins a merge.
class UnionFind {
public:
  explicit UnionFind(unsigned count) : parent(count) {
    for (unsigned i = 0; i < count; ++i)
      parent[i] = i;
  }
  unsigned find(unsigned x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  }
  void unite(unsigned a, unsigned b) {
    a = find(a);
    b = find(b);
    if (a == b)
      return;
    if (a < b)
      parent[b] = a;
    else
      parent[a] = b;
  }

private:
  llvm::SmallVector<unsigned> parent;
};

/// The merged access index over every unit's graphs, all containers
/// deterministically ordered.
struct AccessIndex {
  std::map<std::string, bool> functions; // symbol -> has a definition
  std::map<std::string, bool> globals;   // symbol -> has a definition
  std::map<std::string, std::set<std::string>> calls; // function -> callees
  std::set<std::string> indirect; // functions with a CallsIndirect edge
  // function -> directly read / written globals; address-taking counts as
  // BOTH (the paired ReadsGlobal edge lands in `reads`, and the
  // AddressOfGlobal edge is folded into `writes` — the escaped pointer may
  // be stored through).
  std::map<std::string, std::set<std::string>> reads;
  std::map<std::string, std::set<std::string>> writes;
  // Global-to-global AddressOfGlobal pairs (pointer initializer): pointer
  // and pointee must share an actor.
  std::set<std::pair<std::string, std::string>> globalAddressPairs;
};

/// Parses and merges every unit's graph texts (first-def-wins on `def`,
/// matching the FR-58 merge's first-occurrence dedup), retagging each
/// member's internal-linkage symbols with its running TU ordinal so that
/// file-statics key as (unit, symbol). The parser is the shared
/// `parseItemGraphText` (FR-62 F1a) — one implementation for the planner
/// and the link-side graph merge, so their keying cannot drift. A text
/// that fails to parse contributes nothing (every real input is printer
/// output, so this arm is unreachable short of artifact corruption, and
/// the fail direction — fewer facts, more demotion — is the safe one);
/// its ordinal still advances so later members keep their link positions.
AccessIndex buildAccessIndex(llvm::ArrayRef<ActorUnit> units) {
  AccessIndex index;
  unsigned totalMembers = 0;
  for (const ActorUnit &unit : units)
    totalMembers += unit.graphTexts.size();
  unsigned ordinal = 0;
  for (const ActorUnit &unit : units) {
    for (const std::string &text : unit.graphTexts) {
      std::string parseError;
      mlir::FailureOr<mlir::emitrust::ItemGraph> parsed =
          mlir::emitrust::parseItemGraphText(text, parseError);
      if (mlir::failed(parsed)) {
        ++ordinal;
        continue;
      }
      // A single-member plan keeps the graph's own spellings (a joint
      // source-mode graph already tags each TU's statics distinctly);
      // with several members each text is a single-TU artifact whose
      // `tu0_` tags would collide, so retag by the running ordinal.
      std::map<std::string, std::string> rename;
      if (totalMembers > 1)
        for (const mlir::emitrust::ItemNode &node : parsed->nodes)
          if (node.linkage == mlir::emitrust::ItemLinkage::Internal)
            rename.emplace(node.symbol, mlir::emitrust::retagInternalSymbol(
                                            node.symbol, ordinal));
      auto mapped = [&](const std::string &symbol) {
        auto it = rename.find(symbol);
        return it == rename.end() ? symbol : it->second;
      };
      std::map<std::string, mlir::emitrust::ItemKind> kindOf;
      for (const mlir::emitrust::ItemNode &node : parsed->nodes) {
        kindOf.emplace(node.symbol, node.kind);
        if (node.kind == mlir::emitrust::ItemKind::Function)
          index.functions[mapped(node.symbol)] |= node.isDefinition;
        else if (node.kind == mlir::emitrust::ItemKind::Global)
          index.globals[mapped(node.symbol)] |= node.isDefinition;
      }
      for (const mlir::emitrust::ItemEdge &edge : parsed->edges) {
        auto fromKind = kindOf.find(edge.from);
        bool fromFunction =
            fromKind != kindOf.end() &&
            fromKind->second == mlir::emitrust::ItemKind::Function;
        bool fromGlobal =
            fromKind != kindOf.end() &&
            fromKind->second == mlir::emitrust::ItemKind::Global;
        if (edge.kind == mlir::emitrust::EdgeKind::CallsIndirect) {
          if (fromFunction)
            index.indirect.insert(mapped(edge.from));
          continue;
        }
        if (edge.to.empty())
          continue;
        if (edge.kind == mlir::emitrust::EdgeKind::Calls && fromFunction)
          index.calls[mapped(edge.from)].insert(mapped(edge.to));
        else if (edge.kind == mlir::emitrust::EdgeKind::ReadsGlobal &&
                 fromFunction)
          index.reads[mapped(edge.from)].insert(mapped(edge.to));
        else if (edge.kind == mlir::emitrust::EdgeKind::WritesGlobal &&
                 fromFunction)
          index.writes[mapped(edge.from)].insert(mapped(edge.to));
        else if (edge.kind == mlir::emitrust::EdgeKind::AddressOfGlobal &&
                 fromFunction)
          index.writes[mapped(edge.from)].insert(mapped(edge.to));
        else if (edge.kind == mlir::emitrust::EdgeKind::AddressOfGlobal &&
                 fromGlobal)
          index.globalAddressPairs.emplace(mapped(edge.from),
                                           mapped(edge.to));
      }
      ++ordinal;
    }
  }
  return index;
}

/// Joins sorted `names` as `{a,b,c}` for a note.
std::string braceJoin(llvm::ArrayRef<std::string> names) {
  std::string out = "{";
  for (const std::string &name : names) {
    if (out.size() > 1)
      out += ",";
    out += name;
  }
  out += "}";
  return out;
}

} // namespace

ActorPlan emitrustcc::planActors(
    llvm::ArrayRef<ActorUnit> units,
    llvm::ArrayRef<std::pair<std::string, std::string>> overrides) {
  ActorPlan plan;
  AccessIndex index = buildAccessIndex(units);

  llvm::SmallVector<std::string> defined;
  for (const auto &[symbol, isDef] : index.functions)
    if (isDef)
      defined.push_back(symbol);

  // A hosted sink is a sink only while the project does not define the
  // symbol itself; a project-defined `printf` is an ordinary function.
  auto isSinkCallee = [&](const std::string &symbol) {
    if (!isStdoutSinkName(symbol))
      return false;
    auto it = index.functions.find(symbol);
    return it == index.functions.end() || !it->second;
  };
  std::set<std::string> directStdout;
  for (const auto &[caller, callees] : index.calls)
    for (const std::string &callee : callees)
      if (isSinkCallee(callee))
        directStdout.insert(caller);

  // The universe: every global node plus every access-edge target (edge
  // targets are authoritative even if a node line were missing), plus the
  // "@stdout" pseudo-global when some function calls a hosted sink.
  std::set<std::string> universeSet;
  for (const auto &[symbol, isDef] : index.globals)
    universeSet.insert(symbol);
  for (const auto &[fn, targets] : index.reads)
    universeSet.insert(targets.begin(), targets.end());
  for (const auto &[fn, targets] : index.writes)
    universeSet.insert(targets.begin(), targets.end());
  for (const auto &[from, to] : index.globalAddressPairs) {
    universeSet.insert(from);
    universeSet.insert(to);
  }
  if (!directStdout.empty())
    universeSet.insert(std::string(kStdout));
  llvm::SmallVector<std::string> universe(universeSet.begin(),
                                          universeSet.end());

  auto directReadWrite = [&](const std::string &fn) {
    std::set<std::string> result;
    if (auto it = index.reads.find(fn); it != index.reads.end())
      result.insert(it->second.begin(), it->second.end());
    if (auto it = index.writes.find(fn); it != index.writes.end())
      result.insert(it->second.begin(), it->second.end());
    if (directStdout.count(fn))
      result.insert(std::string(kStdout));
    return result;
  };
  auto directWrite = [&](const std::string &fn) {
    std::set<std::string> result;
    if (auto it = index.writes.find(fn); it != index.writes.end())
      result.insert(it->second.begin(), it->second.end());
    if (directStdout.count(fn))
      result.insert(std::string(kStdout)); // a sink call writes the stream
    return result;
  };

  // -- 1. Overrides pin universe elements to named actors: the LONGEST
  // matching prefix wins, first entry winning a length tie.
  std::map<std::string, std::string> pinned;
  for (const std::string &element : universe) {
    const std::pair<std::string, std::string> *best = nullptr;
    for (const auto &entry : overrides)
      if (llvm::StringRef(element).starts_with(entry.first) &&
          (!best || entry.first.size() > best->first.size()))
        best = &entry;
    if (best)
      pinned.emplace(element, best->second);
  }

  // -- 2. Co-access seeding over the UNPINNED elements: each non-main
  // defined function's direct read|write|address-of footprint is one
  // cluster, and a pointer global joins its pointee's cluster.
  llvm::SmallVector<std::string> unpinned;
  std::map<std::string, unsigned> unpinnedIndex;
  for (const std::string &element : universe)
    if (!pinned.count(element)) {
      unpinnedIndex.emplace(element, unpinned.size());
      unpinned.push_back(element);
    }
  UnionFind seeding(unpinned.size());
  auto uniteAll = [&](const std::set<std::string> &elements) {
    std::optional<unsigned> first;
    for (const std::string &element : elements) {
      auto it = unpinnedIndex.find(element);
      if (it == unpinnedIndex.end())
        continue;
      if (first)
        seeding.unite(*first, it->second);
      else
        first = it->second;
    }
  };
  for (const std::string &fn : defined) {
    if (fn == "c_main")
      continue;
    uniteAll(directReadWrite(fn));
  }
  for (const auto &[from, to] : index.globalAddressPairs)
    uniteAll({from, to});

  // -- 3. Initial assignment: pinned name, else the cluster's smallest
  // member symbol (the sorted index space makes the UF root exactly that).
  std::map<std::string, std::string> actorOf;
  for (const std::string &element : unpinned)
    actorOf.emplace(element,
                    unpinned[seeding.find(unpinnedIndex.at(element))]);
  for (const auto &[element, name] : pinned)
    actorOf[element] = name;

  // -- 4. Condensation over actor names; the smallest name survives.
  llvm::SmallVector<std::string> names;
  {
    std::set<std::string> nameSet;
    for (const auto &[element, name] : actorOf)
      nameSet.insert(name);
    names.assign(nameSet.begin(), nameSet.end());
  }
  std::map<std::string, unsigned> nameIndex;
  for (auto [i, name] : llvm::enumerate(names))
    nameIndex.emplace(name, i);
  UnionFind merged(names.size());
  auto actorsOf = [&](const std::set<std::string> &elements) {
    std::set<std::string> roots;
    for (const std::string &element : elements) {
      auto it = actorOf.find(element);
      if (it != actorOf.end())
        roots.insert(names[merged.find(nameIndex.at(it->second))]);
    }
    return llvm::SmallVector<std::string>(roots.begin(), roots.end());
  };
  auto mergeInto = [&](llvm::ArrayRef<std::string> roots) {
    for (const std::string &root : roots.drop_front())
      merged.unite(nameIndex.at(roots.front()), nameIndex.at(root));
  };

  // R0: a pointer global and its pointee that overrides pinned APART are
  // still one owner (E1's rule); merge them back with a note. A no-op for
  // unpinned pairs, which seeding already clustered silently.
  for (const auto &[from, to] : index.globalAddressPairs) {
    llvm::SmallVector<std::string> roots = actorsOf({from, to});
    if (roots.size() < 2)
      continue;
    plan.notes.push_back(("global '" + from + "' takes the address of "
                          "global '" + to + "'; actors " + braceJoin(roots) +
                          " merged into '" + roots.front() + "'"));
    mergeInto(roots);
  }

  // Calls-closures over defined functions, and the CallsIndirect poison.
  std::map<std::string, std::set<std::string>> closure;
  for (const std::string &fn : defined) {
    std::set<std::string> seen{fn};
    llvm::SmallVector<std::string> work{fn};
    while (!work.empty()) {
      std::string current = work.pop_back_val();
      auto it = index.calls.find(current);
      if (it == index.calls.end())
        continue;
      for (const std::string &callee : it->second)
        if (index.functions.count(callee) && seen.insert(callee).second)
          work.push_back(callee);
    }
    closure.emplace(fn, std::move(seen));
  }
  std::set<std::string> poisoned;
  for (const std::string &fn : defined)
    for (const std::string &member : closure.at(fn))
      if (index.indirect.count(member)) {
        poisoned.insert(fn);
        break;
      }

  std::set<std::string> universeElems(universe.begin(), universe.end());
  auto readWriteFootprint = [&](const std::string &fn) {
    if (poisoned.count(fn))
      return universeElems;
    std::set<std::string> footprint;
    for (const std::string &member : closure.at(fn)) {
      std::set<std::string> direct = directReadWrite(member);
      footprint.insert(direct.begin(), direct.end());
    }
    return footprint;
  };

  // FR-62 slice 4: the surviving merge roots produced by a POISON-driven
  // condensation (a poisoned function's footprint is the whole universe).
  // Tracked as indices into `names` so later merges re-root them; resolved
  // to final actors after condensation. Ordinary writer-rule/SCC merges do
  // not mark — a pure "@stdout" writer merge stays certifiable.
  std::set<unsigned> poisonedRoots;

  // R1: a non-trivial call-graph SCC is one emission unit; if its combined
  // closure footprint spans several actors, they merge. Iterative Tarjan,
  // roots and successors in sorted order for determinism.
  {
    std::set<std::string> definedSet(defined.begin(), defined.end());
    std::map<std::string, unsigned> order, low;
    std::set<std::string> onStack;
    llvm::SmallVector<std::string> stack;
    unsigned counter = 0;
    llvm::SmallVector<llvm::SmallVector<std::string>> components;
    for (const std::string &root : defined) {
      if (order.count(root))
        continue;
      llvm::SmallVector<std::pair<std::string, unsigned>> work;
      auto push = [&](const std::string &fn) {
        order[fn] = low[fn] = counter++;
        stack.push_back(fn);
        onStack.insert(fn);
        work.push_back({fn, 0});
      };
      push(root);
      while (!work.empty()) {
        auto &[fn, next] = work.back();
        llvm::SmallVector<std::string> successors;
        if (auto it = index.calls.find(fn); it != index.calls.end())
          for (const std::string &callee : it->second)
            if (definedSet.count(callee))
              successors.push_back(callee);
        if (next < successors.size()) {
          std::string target = successors[next++];
          if (!order.count(target))
            push(target);
          else if (onStack.count(target))
            low[fn] = std::min(low[fn], order[target]);
          continue;
        }
        std::string finished = fn;
        work.pop_back();
        if (!work.empty())
          low[work.back().first] =
              std::min(low[work.back().first], low[finished]);
        if (low[finished] == order[finished]) {
          llvm::SmallVector<std::string> component;
          while (true) {
            std::string member = stack.pop_back_val();
            onStack.erase(member);
            component.push_back(member);
            if (member == finished)
              break;
          }
          if (component.size() >= 2) {
            llvm::sort(component);
            components.push_back(std::move(component));
          }
        }
      }
    }
    llvm::sort(components);
    for (const llvm::SmallVector<std::string> &component : components) {
      std::set<std::string> footprint;
      bool viaPoison = false;
      for (const std::string &member : component) {
        std::set<std::string> fp = readWriteFootprint(member);
        footprint.insert(fp.begin(), fp.end());
        viaPoison |= poisoned.count(member) != 0;
      }
      llvm::SmallVector<std::string> roots = actorsOf(footprint);
      if (roots.size() < 2)
        continue;
      plan.notes.push_back(
          ("call cycle " + braceJoin(component) + " spans actors " +
           braceJoin(roots) + "; merged into '" + roots.front() + "'"));
      mergeInto(roots);
      if (viaPoison)
        poisonedRoots.insert(merged.find(nameIndex.at(roots.front())));
    }
  }

  // R2: a NON-main defined function whose closure WRITES globals of more
  // than one actor merges them; a poisoned function write-spans the whole
  // universe.
  for (const std::string &fn : defined) {
    if (fn == "c_main")
      continue;
    std::set<std::string> writeFootprint;
    if (poisoned.count(fn)) {
      writeFootprint = universeElems;
    } else {
      for (const std::string &member : closure.at(fn)) {
        std::set<std::string> direct = directWrite(member);
        writeFootprint.insert(direct.begin(), direct.end());
      }
    }
    llvm::SmallVector<std::string> roots = actorsOf(writeFootprint);
    if (roots.size() < 2)
      continue;
    plan.notes.push_back(
        ("function '" + fn + "' " +
         (poisoned.count(fn) ? "indirect-call poison spans actors "
                             : "closure-writes globals of actors ") +
         braceJoin(roots) + "; merged into '" + roots.front() + "'"));
    mergeInto(roots);
    if (poisoned.count(fn))
      poisonedRoots.insert(merged.find(nameIndex.at(roots.front())));
  }

  // -- 5. Final maps.
  std::map<std::string, std::string> finalActorOf;
  std::set<std::string> finalNames;
  for (const std::string &element : universe) {
    std::string name = names[merged.find(nameIndex.at(actorOf.at(element)))];
    finalNames.insert(name);
    finalActorOf.emplace(element, std::move(name));
  }
  std::map<std::string, unsigned> finalIndex;
  for (const std::string &name : finalNames) {
    finalIndex.emplace(name, plan.actors.size());
    Actor actor;
    actor.name = name;
    plan.actors.push_back(std::move(actor));
  }
  for (const std::string &element : universe) {
    unsigned actorIndex = finalIndex.at(finalActorOf.at(element));
    plan.actors[actorIndex].globals.push_back(element);
    plan.actorOfGlobal.push_back({element, actorIndex});
  }
  // FR-62 slice 4: resolve the poison-merge roots to final actors. A
  // recorded root may itself have been merged further; `merged.find`
  // yields whichever name survived.
  for (unsigned root : poisonedRoots) {
    auto it = finalIndex.find(names[merged.find(root)]);
    if (it != finalIndex.end())
      plan.actors[it->second].poisoned = true;
  }

  for (const std::string &fn : defined) {
    ActorFunction assignment;
    assignment.symbol = fn;
    if (fn == "c_main") {
      assignment.role = ActorRole::Driver;
      plan.actorOfFunction.push_back(std::move(assignment));
      continue;
    }
    std::set<std::string> roots;
    for (const std::string &element : readWriteFootprint(fn)) {
      auto it = finalActorOf.find(element);
      if (it != finalActorOf.end())
        roots.insert(it->second);
    }
    if (roots.empty()) {
      assignment.role = ActorRole::Free;
    } else if (roots.size() == 1) {
      assignment.role = ActorRole::Arm;
      assignment.actor = finalIndex.at(*roots.begin());
    } else {
      assignment.role = ActorRole::Cross;
      // FR-62 slice 4: the client's per-actor parameter list. `roots` is a
      // name-sorted std::set and `plan.actors` is name-sorted, so the
      // resulting indices are sorted too — the deterministic parameter
      // order the lift and every call site agree on.
      for (const std::string &root : roots)
        assignment.crossActors.push_back(finalIndex.at(root));
    }
    plan.actorOfFunction.push_back(std::move(assignment));
  }

  return plan;
}

std::string emitrustcc::renderActorPlan(const ActorPlan &plan) {
  std::string text;
  llvm::raw_string_ostream os(text);
  for (const Actor &actor : plan.actors) {
    os << "actor " << actor.name << " globals=";
    llvm::interleave(actor.globals, os, ",");
    os << "\n";
  }
  for (const ActorFunction &fn : plan.actorOfFunction) {
    os << "fn " << fn.symbol << " role=";
    switch (fn.role) {
    case ActorRole::Arm:
      os << "arm actor=" << plan.actors[fn.actor].name;
      break;
    case ActorRole::Driver:
      os << "driver actor=-";
      break;
    case ActorRole::Free:
      os << "free actor=-";
      break;
    case ActorRole::Cross:
      os << "cross actor=-";
      break;
    }
    os << "\n";
  }
  for (const std::string &note : plan.notes)
    os << "note " << note << "\n";
  return text;
}

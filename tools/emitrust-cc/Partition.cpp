//===- Partition.cpp - FR-59 workspace partition planning -----------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements `Partition.h`: parse the units' item-graph texts into a
/// definition index and a unit-level dependency/condensation edge set, seed
/// crates from the directory rule (or the override map), condense until the
/// crate graph is a DAG cargo can express, and name the survivors. The
/// graph shape follows the archived prototype's ConstraintDependencyGraph
/// cherry-pick recorded in design.md: plain indices, explicit cycle
/// detection, deterministic order — no pointer-keyed containers reach any
/// output.
//
//===----------------------------------------------------------------------===//

#include "Partition.h"

#include "CrateEmitter.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <set>

using namespace emitrustcc;

namespace {

/// One parsed `node` line of an item-graph text.
struct GraphNode {
  llvm::StringRef symbol;
  llvm::StringRef kind; // "function" | "record" | "enum" | "global"
  bool isDefinition;
};

/// One parsed `edge` line.
struct GraphEdge {
  llvm::StringRef from;
  llvm::StringRef to; // empty for the `?` CallsIndirect target
  llvm::StringRef kind;
};

/// Parses the two pinned line shapes of `ItemGraph::print` (every field is
/// a whole space-separated token — the format's stated grep contract).
void parseGraphText(llvm::StringRef text,
                    llvm::SmallVectorImpl<GraphNode> &nodes,
                    llvm::SmallVectorImpl<GraphEdge> &edges) {
  for (llvm::StringRef line : llvm::split(text, '\n')) {
    if (line.consume_front("node ")) {
      auto [symbol, rest] = line.split(' ');
      llvm::StringRef kind, def;
      for (llvm::StringRef token : llvm::split(rest, ' ')) {
        if (token.consume_front("kind="))
          kind = token;
        else if (token.consume_front("def="))
          def = token;
      }
      nodes.push_back({symbol, kind, def == "1"});
    } else if (line.consume_front("edge ")) {
      auto [from, rest] = line.split(' ');
      llvm::StringRef arrow, tail;
      std::tie(arrow, tail) = rest.split(' ');
      if (arrow != "->")
        continue;
      auto [to, kindToken] = tail.split(' ');
      llvm::StringRef kind = kindToken;
      kind.consume_front("kind=");
      edges.push_back({from, to == "?" ? llvm::StringRef() : to, kind});
    }
  }
}

/// Union-find with path compression over crate seeds.
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
  /// Merges so that the root with the SMALLER id survives: condensation
  /// always folds a later crate into an earlier one, which is what the
  /// note wordings and the surviving names promise.
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

} // namespace

PartitionPlan emitrustcc::planPartition(
    llvm::ArrayRef<PartitionUnit> units, llvm::StringRef binCrateName,
    llvm::ArrayRef<std::pair<std::string, std::string>> overrides) {
  PartitionPlan plan;
  size_t count = units.size();

  // Parse every unit's graphs once.
  llvm::SmallVector<llvm::SmallVector<GraphNode>> nodes(count);
  llvm::SmallVector<llvm::SmallVector<GraphEdge>> edges(count);
  for (auto [i, unit] : llvm::enumerate(units))
    for (const std::string &text : unit.graphTexts)
      parseGraphText(text, nodes[i], edges[i]);

  // First-definer index (lowest unit wins, matching the merge's
  // first-occurrence dedup of shared-header types), plus the c_main owner.
  llvm::StringMap<std::pair<unsigned, llvm::StringRef>> firstDef;
  std::optional<unsigned> binUnit;
  for (unsigned i = 0; i < count; ++i)
    for (const GraphNode &node : nodes[i]) {
      if (!node.isDefinition)
        continue;
      firstDef.try_emplace(node.symbol, std::make_pair(i, node.kind));
      if (node.symbol == "c_main" && node.kind == "function" && !binUnit)
        binUnit = i;
    }

  // Seed crates: override longest-prefix match on the unit's first source
  // path, else its parent directory. Seeds keyed by (override name | dir
  // path); seed order is first-unit order, which every later ordering
  // inherits.
  llvm::SmallVector<unsigned> unitSeed(count);
  llvm::SmallVector<std::string> seedKeys;    // directory or override tag
  llvm::SmallVector<std::string> seedNames;   // display/package name basis
  llvm::StringMap<unsigned> seedByKey;
  for (unsigned i = 0; i < count; ++i) {
    // NOT a ternary with a "" literal: that would materialize a temporary
    // std::string as the common type and leave the StringRef dangling.
    llvm::StringRef path;
    if (!units[i].sourcePaths.empty())
      path = units[i].sourcePaths.front();
    const std::pair<std::string, std::string> *best = nullptr;
    for (const auto &entry : overrides)
      if (path.starts_with(entry.first) &&
          (!best || entry.first.size() > best->first.size()))
        best = &entry;
    std::string key;
    std::string nameBasis;
    if (best) {
      key = "override:" + best->second;
      nameBasis = best->second;
    } else {
      key = std::string(llvm::sys::path::parent_path(path));
      nameBasis = std::string(llvm::sys::path::filename(
          llvm::sys::path::parent_path(path)));
    }
    auto [it, inserted] = seedByKey.try_emplace(key, seedKeys.size());
    if (inserted) {
      seedKeys.push_back(key);
      seedNames.push_back(nameBasis);
    }
    unitSeed[i] = it->second;
  }
  unsigned seedCount = seedKeys.size();
  UnionFind merged(seedCount);

  // Forced condensations first: globals crossing the boundary (FR-51 never
  // exports a global) and impl blocks away from their type's crate (Rust's
  // orphan rule). Notes are emitted only when the pair is actually split.
  auto condense = [&](unsigned fromSeed, unsigned toSeed,
                      const llvm::Twine &reason) {
    unsigned a = merged.find(fromSeed);
    unsigned b = merged.find(toSeed);
    if (a == b)
      return;
    unsigned survivor = std::min(a, b);
    unsigned absorbed = std::max(a, b);
    plan.notes.push_back(("condensing '" + seedNames[absorbed] + "' into '" +
                          seedNames[survivor] + "': " + reason)
                             .str());
    merged.unite(a, b);
  };

  for (unsigned i = 0; i < count; ++i) {
    for (const GraphEdge &edge : edges[i]) {
      if (edge.to.empty())
        continue;
      auto it = firstDef.find(edge.to);
      if (it == firstDef.end())
        continue;
      auto [defUnit, defKind] = it->second;
      if (defUnit == i)
        continue;
      if (edge.kind == "ReadsGlobal" || edge.kind == "WritesGlobal")
        condense(unitSeed[i], unitSeed[defUnit],
                 "global '" + edge.to +
                     "' cannot cross a crate boundary (globals are never "
                     "exported)");
    }
    for (const std::string &implType : units[i].implTypes) {
      auto it = firstDef.find(implType);
      if (it != firstDef.end() && it->second.first != i)
        condense(unitSeed[i], unitSeed[it->second.first],
                 "impl block for '" + implType +
                     "' must live in its type's crate");
    }
  }

  // Iterate SCC condensation and bin-inbound condensation to a fixpoint:
  // each pass only merges nodes, so the loop terminates.
  auto crateEdges = [&]() {
    std::set<std::pair<unsigned, unsigned>> result;
    for (unsigned i = 0; i < count; ++i)
      for (const GraphEdge &edge : edges[i]) {
        if (edge.to.empty())
          continue;
        auto it = firstDef.find(edge.to);
        if (it == firstDef.end())
          continue;
        unsigned defUnit = it->second.first;
        unsigned from = merged.find(unitSeed[i]);
        unsigned to = merged.find(unitSeed[defUnit]);
        if (from != to)
          result.insert({from, to});
      }
    return result;
  };

  bool changed = true;
  while (changed) {
    changed = false;
    std::set<std::pair<unsigned, unsigned>> graph = crateEdges();

    // Cycle detection by iterative DFS over the condensed crate graph;
    // roots visited in id order for determinism. On finding a back edge,
    // condense the whole cycle into its smallest member and restart.
    std::map<unsigned, llvm::SmallVector<unsigned>> succ;
    std::set<unsigned> vertices;
    for (const auto &[from, to] : graph) {
      succ[from].push_back(to);
      vertices.insert(from);
      vertices.insert(to);
    }
    llvm::DenseSet<unsigned> done;
    for (unsigned root : vertices) {
      if (changed)
        break;
      if (done.contains(root))
        continue;
      llvm::SmallVector<unsigned> stack{root};
      llvm::SmallVector<unsigned> path;
      llvm::DenseSet<unsigned> onPath;
      // Iterative DFS with an explicit color scheme: a sentinel marks
      // "pop path".
      llvm::SmallVector<std::pair<unsigned, unsigned>> work; // (vertex, next)
      work.push_back({root, 0});
      path.push_back(root);
      onPath.insert(root);
      while (!work.empty() && !changed) {
        auto &[vertex, next] = work.back();
        llvm::ArrayRef<unsigned> out = succ[vertex];
        if (next < out.size()) {
          unsigned target = out[next++];
          if (onPath.contains(target)) {
            // Cycle: path suffix from `target` plus the back edge.
            llvm::SmallVector<unsigned> cycle;
            bool in = false;
            for (unsigned v : path) {
              in = in || v == target;
              if (in)
                cycle.push_back(v);
            }
            std::string names;
            for (unsigned v : cycle) {
              if (!names.empty())
                names += " -> ";
              names += seedNames[v];
            }
            unsigned survivor = *llvm::min_element(cycle);
            for (unsigned v : cycle)
              if (v != survivor)
                condense(v, survivor, "dependency cycle " + names);
            changed = true;
            break;
          }
          if (!done.contains(target)) {
            work.push_back({target, 0});
            path.push_back(target);
            onPath.insert(target);
          }
          continue;
        }
        done.insert(vertex);
        onPath.erase(vertex);
        path.pop_back();
        work.pop_back();
      }
    }
    if (changed)
      continue;

    // Bin-inbound: cargo cannot depend on a binary crate, so a crate that
    // references items in the bin crate is condensed into it.
    if (binUnit) {
      unsigned binRoot = merged.find(unitSeed[*binUnit]);
      for (const auto &[from, to] : graph)
        if (to == binRoot && from != binRoot) {
          condense(from, binRoot,
                   "it references items in the binary crate, which cargo "
                   "cannot depend on");
          changed = true;
        }
    }
  }

  // Final crates, ordered by earliest member unit.
  llvm::SmallVector<int> rootCrate(seedCount, -1);
  for (unsigned i = 0; i < count; ++i) {
    unsigned root = merged.find(unitSeed[i]);
    if (rootCrate[root] < 0) {
      rootCrate[root] = static_cast<int>(plan.crates.size());
      PartitionCrate crate;
      crate.isBin = binUnit && merged.find(unitSeed[*binUnit]) == root;
      crate.name = crate.isBin ? std::string(binCrateName)
                               : sanitizeCrateName(seedNames[root]);
      plan.crates.push_back(std::move(crate));
    }
    unsigned crateIndex = static_cast<unsigned>(rootCrate[root]);
    plan.unitCrate.push_back(crateIndex);
    plan.crates[crateIndex].units.push_back(i);
  }

  // Disambiguate name collisions deterministically.
  llvm::StringMap<unsigned> nameUses;
  for (PartitionCrate &crate : plan.crates) {
    unsigned &uses = nameUses[crate.name];
    if (++uses > 1)
      crate.name += "_" + std::to_string(uses);
  }

  // Crate-level deps from the final assignment.
  for (unsigned i = 0; i < count; ++i)
    for (const GraphEdge &edge : edges[i]) {
      if (edge.to.empty())
        continue;
      auto it = firstDef.find(edge.to);
      if (it == firstDef.end())
        continue;
      unsigned from = plan.unitCrate[i];
      unsigned to = plan.unitCrate[it->second.first];
      if (from != to &&
          !llvm::is_contained(plan.crates[from].deps, to))
        plan.crates[from].deps.push_back(to);
    }
  for (PartitionCrate &crate : plan.crates)
    llvm::sort(crate.deps);

  return plan;
}

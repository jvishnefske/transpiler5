//===- FrontierSearch.cpp - bounded best-first port search ------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements FR-43's frontier tree search. See FrontierSearch.h for the
/// model, the scoring rule, the determinism argument, and the trace format.
///
/// The whole file is a pure function of its inputs plus the injected
/// `ProbeFn`: no filesystem, no clock, no global state, no MLIR context. The
/// imperative half — running an import, lowering it, counting what came out —
/// lives in the driver (tools/emitrust-cc), which is what makes this
/// algorithm testable on a synthetic probe and what keeps a project analysis
/// library free of the importer's IR-building machinery.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Project/FrontierSearch.h"

#include "EmitRust/ClangProjectParser.h"

#include "clang/Frontend/ASTUnit.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <deque>
#include <memory>
#include <tuple>
#include <utility>

using namespace mlir;
using namespace mlir::emitrust;

//===----------------------------------------------------------------------===//
// The representation dimension
//===----------------------------------------------------------------------===//

std::vector<RepresentationChoice>
mlir::emitrust::enumerateRepresentations(const ItemNode &item) {
  // ONE candidate, deliberately: the greedy Pass-A planning the importer
  // already performs. With a single candidate every state agrees on every
  // item's representation, `repCost` is uniformly zero, and the "re-pick a
  // representation" branch of expansion generates nothing — which is exactly
  // why this wave cannot change any existing output.
  //
  // The item is not consulted yet. It is a parameter because the first real
  // alternative is per-item and structural: FR-39's container fat-op split
  // (array pool vs `Vec` vs `VecDeque`, W4.5) is chosen per POOL, and the
  // pool is an item. When that becomes selectable this function grows a
  // `switch` on `item.kind` plus a lookup of the item's planned shape, and
  // nothing else in this file changes.
  (void)item;
  return {RepresentationChoice{"default", 0}};
}

//===----------------------------------------------------------------------===//
// States
//===----------------------------------------------------------------------===//

bool SearchState::admits(llvm::StringRef symbol) const {
  return std::binary_search(admitted.begin(), admitted.end(), symbol.str());
}

std::string SearchState::signature() const {
  // Length-prefixed rather than separator-joined: a symbol cannot contain a
  // separator today, but a key that is injective for ALL strings costs one
  // integer per item and removes the question entirely. The representation
  // index rides in the same record, so two states differing only in a
  // representation choice are distinct keys.
  std::string key;
  llvm::raw_string_ostream os(key);
  for (auto [index, symbol] : llvm::enumerate(admitted)) {
    os << symbol.size() << ':' << symbol << '@'
       << (index < representation.size() ? representation[index] : 0) << ';';
  }
  return key;
}

namespace {

/// A symbol-to-node index over a graph. Built once per use rather than
/// scanning `nodes` per lookup, so the search stays linear-ish in the item
/// count on a project with thousands of items.
std::map<llvm::StringRef, const ItemNode *> indexNodes(const ItemGraph &graph) {
  std::map<llvm::StringRef, const ItemNode *> index;
  for (const ItemNode &node : graph.nodes)
    index[node.symbol] = &node;
  return index;
}

} // namespace

unsigned SearchState::representationCost(const ItemGraph &graph) const {
  std::map<llvm::StringRef, const ItemNode *> index = indexNodes(graph);
  unsigned cost = 0;
  for (auto [position, symbol] : llvm::enumerate(admitted)) {
    auto entry = index.find(symbol);
    if (entry == index.end())
      continue;
    std::vector<RepresentationChoice> choices =
        enumerateRepresentations(*entry->second);
    unsigned pick =
        position < representation.size() ? representation[position] : 0;
    if (pick < choices.size())
      cost += choices[pick].cost;
  }
  return cost;
}

std::vector<std::string> mlir::emitrust::searchRoots(const ItemGraph &graph) {
  std::vector<std::string> roots;
  for (const ItemNode &node : graph.nodes) {
    // A prototype is not a root: there is nothing there to port, and FR-44's
    // denominator excludes it for the same reason.
    if (!node.isDefinition)
      continue;
    // `main` is a root because it is the program's entry point; every other
    // externally visible DEFINITION is a root because a partial port is a
    // library as much as a program — an exported function nobody in this
    // project calls is still something the port owes its consumers.
    if (node.symbol == "c_main" || node.linkage == ItemLinkage::External)
      roots.push_back(node.symbol);
  }
  llvm::sort(roots);
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  return roots;
}

std::map<std::string, unsigned>
mlir::emitrust::rootDistances(const ItemGraph &graph,
                              llvm::ArrayRef<std::string> roots) {
  std::map<std::string, unsigned> distance;
  for (const ItemNode &node : graph.nodes)
    distance[node.symbol] = UINT_MAX;
  // Adjacency built once. `graph.edges` is sorted by (from, kind, to), so
  // each successor list comes out in that order and the traversal is a
  // function of content alone.
  std::map<std::string, std::vector<std::string>> successors;
  for (const ItemEdge &edge : graph.edges)
    if (!edge.to.empty())
      successors[edge.from].push_back(edge.to);
  // Breadth-first, seeded with every root at once.
  std::deque<std::string> queue;
  for (const std::string &root : roots) {
    auto entry = distance.find(root);
    if (entry == distance.end() || entry->second == 0)
      continue;
    entry->second = 0;
    queue.push_back(root);
  }
  while (!queue.empty()) {
    std::string current = queue.front();
    queue.pop_front();
    unsigned next = distance[current] + 1;
    auto adjacent = successors.find(current);
    if (adjacent == successors.end())
      continue;
    for (const std::string &target : adjacent->second) {
      auto entry = distance.find(target);
      if (entry == distance.end() || entry->second <= next)
        continue;
      entry->second = next;
      queue.push_back(target);
    }
  }
  return distance;
}

namespace {

/// Builds a state from a coloring: every Green or Yellow item, at its default
/// representation. The one place a coloring becomes an admitted set, used for
/// the root and for every re-coloring after a drop, so the two can never
/// disagree about what "admissible" means.
SearchState stateFromColoring(const ItemColoring &coloring) {
  SearchState state;
  for (const ColoredItem &item : coloring.items)
    if (item.color != ItemColor::Red)
      state.admitted.push_back(item.symbol);
  // `coloring.items` is already in symbol order; sorting is a no-op that
  // makes the postcondition local rather than inherited.
  llvm::sort(state.admitted);
  state.representation.assign(state.admitted.size(), 0);
  return state;
}

/// The admissibility seeds of `base` plus one synthetic inadmissible seed per
/// dropped symbol.
///
/// `signatureLevel` is false for a search drop on purpose: a dropped FUNCTION
/// keeps its mapped signature, so FR-42 will emit an `unimplemented!()` stub
/// for it and its callers stay Yellow and admitted. That is the optimistic
/// reading, and it is safe here for the same reason the coloring probe's
/// optimism is safe — a state that turns out not to import is not a wrong
/// answer, it is the next probe's input.
ItemAdmissibility seedsWithDrops(const ItemAdmissibility &base,
                                 const std::set<std::string> &drops) {
  ItemAdmissibility seeds = base;
  for (const std::string &symbol : drops)
    seeds.reject(symbol, "search-drop", /*signatureLevel=*/false);
  return seeds;
}

/// One state waiting to be probed.
struct Pending {
  /// The parent's score, the heuristic this is ordered by.
  SearchScore parentScore;
  /// The parent's node id.
  unsigned parent = 0;
  /// The symbol dropped relative to the parent.
  std::string drop;
  /// `learned` or `blamed`.
  std::string why;
  /// How complete a repair this is; see `Candidate::rank`.
  unsigned repairRank = 1;
  /// `drop`'s distance from the nearest root; `UINT_MAX` when no root
  /// reaches it.
  unsigned dropDistance = UINT_MAX;
  /// The accumulated drop set, including `drop`.
  std::set<std::string> drops;
  /// The state itself.
  SearchState state;
  /// Its memoization key, computed once.
  std::string signature;
};

/// The frontier's total order, in five keys:
///
///  1. the best PARENT SCORE — best-first over what has actually been
///     measured, since a child's own score is not known until it is probed;
///  2. the CLASS-COMPLETE repair first (`Candidate::rank`) — a partial repair
///     of a whole-program failure usually just fails again, and paying an
///     import to discover that is the one cost worth avoiding;
///  3. the LARGEST admitted set — among equally complete repairs the smallest
///     one first, because the score maximizes items emitted;
///  4. the dropped item FURTHEST from a root — when two repairs cost the same
///     number of items, give up the one the project's entry points depend on
///     least (this is the one place FR-43's ROOTS enter the algorithm; see
///     FrontierSearch.h for why they are not the root state);
///  5. the signature.
///
/// No tie is left for insertion order to break, which is what makes the
/// search reproducible under a permutation of the input file order.
bool pendingBefore(const Pending &lhs, const Pending &rhs) {
  if (lhs.parentScore.isBetterThan(rhs.parentScore))
    return true;
  if (rhs.parentScore.isBetterThan(lhs.parentScore))
    return false;
  if (lhs.repairRank != rhs.repairRank)
    return lhs.repairRank < rhs.repairRank;
  if (lhs.state.admitted.size() != rhs.state.admitted.size())
    return lhs.state.admitted.size() > rhs.state.admitted.size();
  if (lhs.dropDistance != rhs.dropDistance)
    return lhs.dropDistance > rhs.dropDistance;
  return lhs.signature < rhs.signature;
}

/// A repair to try: the items to give up, a label for the trace, where the
/// idea came from, and how complete a repair it is.
struct Candidate {
  /// The symbols to drop, sorted and unique. Usually one.
  std::vector<std::string> drops;
  /// The trace's `drop=` token: the single symbol, or `<first>+<k>` when the
  /// repair gives up `k` more items alongside it.
  std::string label;
  /// `learned` or `blamed`.
  std::string why;
  /// 0 for a CLASS-COMPLETE repair — one that removes every item the graph
  /// can identify as a possible cause of this failure, so a project with ten
  /// such items is repaired by one probe rather than ten. 1 for a partial
  /// one. Ordered ahead of everything but the parent score, because a
  /// partial repair of a whole-program failure usually just fails again.
  unsigned rank = 1;
};

/// Builds a candidate from a set of symbols, deriving its label.
Candidate makeCandidate(std::vector<std::string> drops, llvm::StringRef why,
                        unsigned rank) {
  llvm::sort(drops);
  drops.erase(std::unique(drops.begin(), drops.end()), drops.end());
  std::string label = drops.empty() ? std::string() : drops.front();
  if (drops.size() > 1)
    label += "+" + std::to_string(drops.size() - 1);
  return Candidate{std::move(drops), std::move(label), why.str(), rank};
}

/// Orders repairs: class-complete ones first, then FURTHEST-FROM-A-ROOT
/// first, then by label. An item no root reaches has distance `UINT_MAX` and
/// is tried first: it is the least load-bearing item in the project, so
/// removing it costs the port the least. This is the one place the ROOTS
/// enter the algorithm (see FrontierSearch.h for why they are not the root
/// state).
unsigned candidateDistance(const Candidate &candidate,
                           const std::map<std::string, unsigned> &distance) {
  // A multi-item repair is as load-bearing as its most load-bearing member.
  unsigned nearest = UINT_MAX;
  for (const std::string &symbol : candidate.drops) {
    auto entry = distance.find(symbol);
    unsigned own = entry == distance.end() ? UINT_MAX : entry->second;
    nearest = std::min(nearest, own);
  }
  return nearest;
}

void sortCandidates(std::vector<Candidate> &candidates,
                    const std::map<std::string, unsigned> &distance) {
  llvm::stable_sort(candidates, [&](const Candidate &lhs,
                                    const Candidate &rhs) {
    if (lhs.rank != rhs.rank)
      return lhs.rank < rhs.rank;
    unsigned lhsDistance = candidateDistance(lhs, distance);
    unsigned rhsDistance = candidateDistance(rhs, distance);
    if (lhsDistance != rhsDistance)
      return lhsDistance > rhsDistance;
    return lhs.label < rhs.label;
  });
  // Deduplicate by the dropped set, keeping the first (best-ordered)
  // occurrence and therefore its `why` and rank.
  std::vector<Candidate> unique;
  for (Candidate &candidate : candidates) {
    if (candidate.drops.empty())
      continue;
    bool seen = llvm::any_of(unique, [&](const Candidate &kept) {
      return kept.drops == candidate.drops;
    });
    if (!seen)
      unique.push_back(std::move(candidate));
  }
  candidates = std::move(unique);
}

/// The repairs that could fix a FAILED probe.
///
/// Two attributions, and the first is the one that matters.
///
///  1. The failure NAMES a symbol — `finalizeProject`'s "'g' is referenced but
///     not defined in any translation unit", the canonical way a recovering
///     import still dies whole-program. Three repairs come out of it:
///
///      a. CLASS-COMPLETE (rank 0): drop every admitted item the project only
///         DECLARES. A declaration with no definition anywhere is precisely
///         what this failure is about, and the graph knows all of them, so
///         one probe settles a project that calls ten library functions
///         instead of ten probes settling them one at a time. It is also what
///         makes the search ORDER-INDEPENDENT here: `finalizeProject` returns
///         at the FIRST undefined symbol it meets, and which one that is
///         depends on the translation-unit order — but the set does not.
///      b. The named symbol itself, when the project does define it and the
///         import rejected that definition. The class-complete repair does
///         not cover this case, because the item is not a mere declaration.
///      c. Each admitted REFERRER of anything in (a) or (b). Giving up a
///         caller keeps the callee, which is the opposite trade and sometimes
///         the better one; the score decides, which is the whole reason both
///         are generated instead of stopping at the first repair that works.
///
///  2. The failure only has a LOCATION. The enclosing item is the last graph
///     node in that file at or before the failing line: node locations are
///     declaration starts, so the greatest one not past the failure is the
///     declaration the failure is inside.
std::vector<Candidate> blameCandidates(const ItemGraph &graph,
                                       const SearchState &state,
                                       const ProbeOutcome &outcome) {
  std::vector<Candidate> candidates;
  if (!outcome.failureSymbol.empty()) {
    std::vector<std::string> undefined;
    for (const ItemNode &node : graph.nodes)
      if (!node.isDefinition && state.admits(node.symbol))
        undefined.push_back(node.symbol);
    std::vector<std::string> blamed = undefined;
    if (state.admits(outcome.failureSymbol) &&
        !llvm::is_contained(undefined, outcome.failureSymbol)) {
      blamed.push_back(outcome.failureSymbol);
      candidates.push_back(
          makeCandidate({outcome.failureSymbol}, "blamed", /*rank=*/1));
    }
    if (!undefined.empty())
      candidates.push_back(makeCandidate(undefined, "blamed", /*rank=*/0));
    for (const ItemEdge &edge : graph.edges)
      if (llvm::is_contained(blamed, edge.to) && state.admits(edge.from))
        candidates.push_back(makeCandidate({edge.from}, "blamed", /*rank=*/1));
    if (!candidates.empty())
      return candidates;
  }
  if (!outcome.failureFile.empty() && outcome.failureLine != 0) {
    const ItemNode *best = nullptr;
    for (const ItemNode &node : graph.nodes) {
      if (node.file != outcome.failureFile || node.line > outcome.failureLine)
        continue;
      if (!state.admits(node.symbol))
        continue;
      if (!best || node.line > best->line ||
          (node.line == best->line && node.symbol < best->symbol))
        best = &node;
    }
    if (best)
      candidates.push_back(makeCandidate({best->symbol}, "blamed", /*rank=*/1));
  }
  return candidates;
}

/// Renders one `key=value` trace line's worth of a state's score.
void printScore(llvm::raw_ostream &os, const SearchScore &score) {
  os << " ported=" << score.ported << " stubbed=" << score.stubbed
     << " rep-cost=" << score.repCost;
}

} // namespace

SearchState mlir::emitrust::rootState(const ItemGraph &graph,
                                      const ItemColoring &coloring) {
  (void)graph;
  return stateFromColoring(coloring);
}

SearchState mlir::emitrust::baselineState(const ItemGraph &graph) {
  SearchState state;
  for (const ItemNode &node : graph.nodes)
    state.admitted.push_back(node.symbol);
  // `graph.nodes` is already in symbol order; sorting makes `admits`'s binary
  // search a local postcondition rather than an inherited one, exactly as
  // `stateFromColoring` does.
  llvm::sort(state.admitted);
  state.admitted.erase(std::unique(state.admitted.begin(),
                                   state.admitted.end()),
                       state.admitted.end());
  state.representation.assign(state.admitted.size(), 0);
  return state;
}

//===----------------------------------------------------------------------===//
// Scoring
//===----------------------------------------------------------------------===//

bool SearchScore::isBetterThan(const SearchScore &other) const {
  // A state that does not import at all loses to one that does, at any
  // counts: FR-43 maximizes items EMITTED, and a failed import emits nothing.
  if (imported != other.imported)
    return imported;
  if (ported != other.ported)
    return ported > other.ported;
  if (stubbed != other.stubbed)
    return stubbed < other.stubbed;
  return repCost < other.repCost;
}

//===----------------------------------------------------------------------===//
// The search
//===----------------------------------------------------------------------===//

SearchResult mlir::emitrust::frontierSearch(const ItemGraph &graph,
                                            const ItemAdmissibility &admissibility,
                                            const SearchOptions &options,
                                            const ProbeFn &probe) {
  SearchResult result;
  std::string traceText;
  llvm::raw_string_ostream trace(traceText);

  std::vector<std::string> roots = searchRoots(graph);
  std::map<std::string, unsigned> distance = rootDistances(graph, roots);
  unsigned budget = std::max(options.maxNodes, 1u);

  // FR-50's safety net. The baseline is the plain recovering import expressed
  // as a state, and probing it is what makes "the search is never worse than
  // not searching" true by construction rather than by trusting FR-41's
  // coloring. When the coloring rules nothing out the two states coincide and
  // nothing below fires, so an exact project still costs exactly one probe.
  SearchState baseline = baselineState(graph);
  ItemColoring rootColoring = computeColoring(graph, admissibility);
  SearchState root = rootState(graph, rootColoring);
  std::string baselineSignature = baseline.signature();
  bool baselineIsRoot = baselineSignature == root.signature();
  // The guarantee is not subject to `--max-search-nodes`: a budget of one
  // would probe the root and stop, leaving the baseline unmeasured and the
  // postcondition unwitnessed. Two probes is the floor exactly when there is
  // a second state to probe.
  if (!baselineIsRoot)
    budget = std::max(budget, 2u);

  trace << "search items=" << graph.nodes.size() << " roots=" << roots.size()
        << " max-nodes=" << budget << "\n";

  // The accumulated drop set of each probed node, parallel to
  // `result.nodes`. Kept here rather than on the published `SearchNode`
  // because it is derivation scratch: the state's `admitted` is the answer,
  // and the drops are only how it was reached.
  std::vector<std::set<std::string>> nodeDrops;

  trace << "root 0 admitted=" << root.admitted.size()
        << " excluded=" << (graph.nodes.size() - root.admitted.size()) << "\n";

  std::set<std::string> seen;
  std::vector<Pending> frontier;
  frontier.push_back(Pending{SearchScore{}, /*parent=*/0, /*drop=*/"",
                             /*why=*/"", /*repairRank=*/0,
                             /*dropDistance=*/UINT_MAX,
                             /*drops=*/{}, root, root.signature()});
  // The root's heuristic must beat every child's, and a child's is its
  // parent's REAL score, which is at best `imported`. Marking the root's
  // pending score as imported with an unreachable ported count would be a
  // lie in the trace; instead the root is simply the only entry at the start,
  // so no comparison against it is ever made.
  seen.insert(frontier.front().signature);
  result.generated = 1;

  while (!frontier.empty()) {
    if (result.nodes.size() >= budget) {
      result.stopReason = "node-budget";
      break;
    }
    auto best = std::min_element(frontier.begin(), frontier.end(),
                                 [](const Pending &lhs, const Pending &rhs) {
                                   return pendingBefore(lhs, rhs);
                                 });
    Pending current = std::move(*best);
    frontier.erase(best);

    unsigned id = static_cast<unsigned>(result.nodes.size());
    SearchNode node;
    node.id = id;
    node.parent = current.parent;
    node.drop = current.drop;
    node.why = current.why;
    node.state = current.state;
    node.outcome = probe(current.state);
    node.score = SearchScore{node.outcome.imported, node.outcome.ported,
                             node.outcome.stubbed,
                             current.state.representationCost(graph)};

    if (node.why == "baseline")
      trace << "baseline " << id << " admitted=" << node.state.admitted.size()
            << " excluded="
            << (graph.nodes.size() - node.state.admitted.size()) << "\n";
    else if (id != 0)
      trace << "child " << id << " from=" << node.parent
            << " drop=" << node.drop << " cascade="
            << (result.nodes[node.parent].state.admitted.size() -
                node.state.admitted.size())
            << " why=" << node.why << "\n";
    trace << "probe " << id
          << " outcome=" << (node.outcome.imported ? "ok" : "failed");
    printScore(trace, node.score);
    trace << " dropped=" << node.outcome.dropped << "\n";
    // The ledger records one entry per TRANSLATION UNIT that reached the
    // declaration, so a header's item rejects once per including TU; and its
    // order is the declaration-walk order, which a permutation of the input
    // file list changes. Both are dropped here — deduplicated, then sorted on
    // content — because neither carries information the search can use and
    // both would make the trace depend on something other than the project.
    {
      std::vector<std::tuple<std::string, bool, std::string, bool>> learned;
      for (const LearnedRejection &rejection : node.outcome.rejections)
        learned.push_back({rejection.symbol, rejection.stubbed,
                           rejection.blockerTag,
                           node.state.admits(rejection.symbol)});
      llvm::sort(learned);
      learned.erase(std::unique(learned.begin(), learned.end()), learned.end());
      for (const auto &[symbol, stubbed, tag, actionable] : learned)
        trace << "learn " << id << " rejected=" << symbol
              << " as=" << (stubbed ? "stub" : "drop") << " tag=" << tag
              << " new=" << (actionable ? "yes" : "no") << "\n";
    }
    if (!node.outcome.imported)
      trace << "learn " << id << " failure="
            << (node.outcome.failureSymbol.empty() ? "-"
                                                   : node.outcome.failureSymbol)
            << " reason=" << node.outcome.failure << "\n";

    if (id == 0 || node.score.isBetterThan(result.bestScore)) {
      result.best = node.state;
      result.bestScore = node.score;
      result.bestNode = id;
    }
    // The baseline's score is remembered as the FR-50 witness. It needs no
    // special treatment above — it competed for `best` on the same terms as
    // every other state, which is the whole point — so all that is kept here
    // is the value the postcondition is stated against. When the coloring
    // ruled nothing out the root IS the baseline, and the witness is the
    // root's own score: leaving it default-constructed would state the
    // postcondition against a score no state ever had.
    if (node.why == "baseline" || (id == 0 && baselineIsRoot)) {
      result.baselineScore = node.score;
      result.baselineNode = id;
    }
    result.nodes.push_back(std::move(node));
    nodeDrops.push_back(current.drops);
    const SearchNode &probed = result.nodes.back();

    // The mandatory baseline probe is queued the moment the root has a real
    // score to order it by. It lands at node 1: it shares the root's score
    // with every child the root generates, ties them on rank, and then wins
    // on the largest-admitted-set key, because it admits every item in the
    // project and a child of the root admits strictly fewer than the root.
    if (id == 0 && !baselineIsRoot) {
      frontier.push_back(Pending{probed.score, /*parent=*/0, /*drop=*/"",
                                 /*why=*/"baseline", /*repairRank=*/0,
                                 /*dropDistance=*/UINT_MAX,
                                 /*drops=*/{}, baseline, baselineSignature});
      seen.insert(baselineSignature);
      ++result.generated;
    }

    // Candidates. A successful probe teaches its rejections; a failed one is
    // attributed by blame. Both are ordered by the same rule.
    std::vector<Candidate> candidates;
    if (probed.outcome.imported) {
      for (const LearnedRejection &rejection : probed.outcome.rejections)
        if (probed.state.admits(rejection.symbol))
          candidates.push_back(
              makeCandidate({rejection.symbol}, "learned", /*rank=*/1));
    } else {
      candidates = blameCandidates(graph, probed.state, probed.outcome);
    }
    sortCandidates(candidates, distance);

    for (const Candidate &candidate : candidates) {
      std::set<std::string> drops = nodeDrops[id];
      bool added = false;
      for (const std::string &symbol : candidate.drops)
        added |= drops.insert(symbol).second;
      if (!added)
        continue; // Already dropped on this path; the child would be `this`.
      ItemAdmissibility seeds = seedsWithDrops(admissibility, drops);
      SearchState child = stateFromColoring(computeColoring(graph, seeds));
      std::string signature = child.signature();
      ++result.generated;
      if (child.admitted.size() == probed.state.admitted.size() &&
          signature == current.signature) {
        ++result.pruned;
        trace << "prune " << result.generated - 1 << " from=" << id
              << " drop=" << candidate.label << " why=empty\n";
        continue;
      }
      if (!seen.insert(signature).second) {
        ++result.pruned;
        trace << "prune " << result.generated - 1 << " from=" << id
              << " drop=" << candidate.label << " why=memoized\n";
        continue;
      }
      frontier.push_back(Pending{probed.score, id, candidate.label,
                                 candidate.why, candidate.rank,
                                 candidateDistance(candidate, distance),
                                 std::move(drops), std::move(child),
                                 std::move(signature)});
    }
  }
  if (result.stopReason.empty())
    result.stopReason = "exhausted";

  trace << "stop reason=" << result.stopReason << "\n";
  trace << "best " << result.bestNode;
  printScore(trace, result.bestScore);
  trace << "\n";
  std::map<llvm::StringRef, const ItemNode *> nodeIndex = indexNodes(graph);
  for (auto [position, symbol] : llvm::enumerate(result.best.admitted)) {
    auto entry = nodeIndex.find(symbol);
    llvm::StringRef repName = "default";
    std::vector<RepresentationChoice> choices;
    if (entry != nodeIndex.end()) {
      choices = enumerateRepresentations(*entry->second);
      unsigned pick = position < result.best.representation.size()
                          ? result.best.representation[position]
                          : 0;
      if (pick < choices.size())
        repName = choices[pick].name;
    }
    trace << "admitted " << symbol << " rep=" << repName << "\n";
  }
  // The excluded items, with WHY each is out. Three answers, and the whole
  // value of the trace to a person asking "why is this item not in my crate?"
  // is that they are different questions with different fixes:
  //  - `red`     FR-41's coloring ruled it out before any import ran. The fix
  //              is to support the construct its blame chain names.
  //  - `dropped` the search removed it in response to something a probe
  //              learned. The fix is to support what that probe rejected.
  //  - `cascade` nothing is wrong with it: it was Green or Yellow at the root
  //              and lost its color only because a `dropped` item it depends
  //              on took it down. The fix is somebody else's item.
  std::set<std::string> searchDrops =
      result.bestNode < nodeDrops.size() ? nodeDrops[result.bestNode]
                                         : std::set<std::string>();
  for (const ItemNode &node : graph.nodes) {
    if (result.best.admits(node.symbol))
      continue;
    llvm::StringRef why = "red";
    if (searchDrops.count(node.symbol))
      why = "dropped";
    else if (root.admits(node.symbol))
      why = "cascade";
    trace << "excluded " << node.symbol << " why=" << why << "\n";
  }
  trace << "summary probes=" << result.nodes.size()
        << " generated=" << result.generated << " pruned=" << result.pruned
        << " improved=" << (result.improved() ? "yes" : "no")
        << " >=baseline=" << (result.atLeastBaseline() ? "yes" : "no") << "\n";

  // FR-50's postcondition, asserted rather than repaired. `best` is a maximum
  // over the probed states and the baseline is one of them, so a violation
  // cannot be fixed by falling back — it would mean the probe or the score is
  // not a function of the state, and the honest response to that is to fail
  // loudly rather than to hide it behind a substitute answer. The trace
  // carries the same verdict for release builds and for anyone reading the
  // output after the fact.
  assert(result.atLeastBaseline() &&
         "FR-50: the frontier search scored below the unrestricted import");

  result.trace = traceText;
  return result;
}

//===----------------------------------------------------------------------===//
// Inputs
//===----------------------------------------------------------------------===//

std::set<std::string>
mlir::emitrust::excludedItemsFor(const ItemGraph &graph,
                                 const SearchState &state) {
  std::set<std::string> excluded;
  for (const ItemNode &node : graph.nodes)
    if (!state.admits(node.symbol))
      excluded.insert(node.symbol);
  return excluded;
}

FailureOr<SearchInputs>
mlir::emitrust::buildSearchInputs(llvm::ArrayRef<std::string> paths,
                                  llvm::ArrayRef<std::string> extraClangArgs,
                                  llvm::StringRef compilationDatabasePath,
                                  std::string &error) {
  // One parse for all three analyses, exactly as `colorItems` does and for
  // the same reason: two parses could disagree about what the project
  // contains, and the search's whole vocabulary is the graph's node keys.
  std::vector<std::unique_ptr<clang::ASTUnit>> owned;
  std::vector<std::string> sources;
  // FR-68: the attribution is unused here — a driver-level error still fails
  // the build through the hardened status below, and clang has already
  // printed the diagnostic itself; only the importer entry points relocate
  // it onto the offending TU.
  ProjectParseError firstClangError;
  int status =
      buildProjectASTs(paths, extraClangArgs, compilationDatabasePath, owned,
                       sources, error, firstClangError);
  if (!error.empty())
    return failure();
  if (owned.size() != sources.size() || status != 0)
    return failure();
  llvm::SmallVector<clang::ASTUnit *, 4> units;
  for (const std::unique_ptr<clang::ASTUnit> &unit : owned) {
    if (!unit || unit->getDiagnostics().hasErrorOccurred())
      return failure();
    units.push_back(unit.get());
  }

  SearchInputs inputs;
  FailureOr<ItemGraph> graph =
      buildItemGraph(llvm::ArrayRef<clang::ASTUnit *>(units));
  if (failed(graph))
    return failure();
  FailureOr<ItemAdmissibility> admissibility =
      probeAdmissibility(llvm::ArrayRef<clang::ASTUnit *>(units));
  if (failed(admissibility))
    return failure();
  inputs.graph = std::move(*graph);
  inputs.admissibility = std::move(*admissibility);
  inputs.coloring = computeColoring(inputs.graph, inputs.admissibility);
  return inputs;
}

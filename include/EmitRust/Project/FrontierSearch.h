//===- FrontierSearch.h - bounded best-first port search --------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// FR-43: the driver that turns FR-40's item graph, FR-41's coloring and
/// FR-42's recoverable import into a MAXIMAL PARTIAL PORT.
///
/// What this is for. FR-42 made a project with unsupported items yield a
/// crate instead of nothing, by rejecting item by item as the declaration
/// walk reaches them. That is a GREEDY, single-attempt answer: whatever the
/// first walk happens to accept is the result, and it is the result even when
/// the walk fails outright — a rejected item can leave the project referencing
/// a symbol nobody defines, which is a whole-program fact `finalizeProject`
/// still turns into a hard error, so `--incremental` produces NO crate at all.
/// This file adds the second attempt, and the third: a bounded best-first
/// search over SETS of admitted items, where every import attempt is a probe
/// whose rejections are facts fed back into the next candidate.
///
///===--------------------------------------------------------------------===//
/// The state space
///===--------------------------------------------------------------------===//
///
/// A `SearchState` is a set of ADMITTED items (FR-40 node keys) plus one
/// REPRESENTATION choice per admitted item. Probing a state means importing
/// the whole project with the state's complement excluded
/// (`ImportOptions::excludedItems`); an excluded item takes FR-42's ordinary
/// recovery path, so it becomes a signature-preserving stub when it can and
/// is dropped when it cannot. A state is therefore never a module the
/// importer does not already know how to build.
///
/// The ROOT state admits every item FR-41 colored Green or Yellow.
///
///===--------------------------------------------------------------------===//
/// The safety net: the search may not lose to not searching
///===--------------------------------------------------------------------===//
///
/// FR-50. The root state is derived from FR-41's coloring, and a coloring is
/// an approximation. If it calls an importable item Red, that item is missing
/// from the root state — and since every child of a state ADMITS STRICTLY
/// LESS than its parent, no amount of searching can put it back. A single
/// false Red therefore made `--search` produce a SMALLER crate than plain
/// `--incremental`, which is the opposite of the feature's purpose.
///
/// The fix is not to trust the coloring more. It is to notice that the plain
/// recovering import is ITSELF a state in this space — the one that admits
/// EVERY item, whose `excludedItems` is empty — and that a search which
/// evaluates that state and keeps the best result cannot come out behind it.
/// So `frontierSearch` probes it, as node 1, immediately after the root:
///
///  - It is a MANDATORY probe, not a candidate that the frontier order might
///    or might not get to. The budget is raised to at least 2 when the
///    baseline differs from the root, because "never worse than not
///    searching" is a guarantee and a guarantee cannot be subject to a knob.
///    When the coloring rules NOTHING out the two states are identical, the
///    baseline is memoized away, and the search costs exactly what it used to.
///  - It is an ordinary state in every other respect. Its score is compared
///    with the same lexicographic rule, its rejections teach the same way
///    (and they teach the most, since it admitted everything), and its
///    children are generated the same way. Nothing about it is special-cased
///    downstream of its being probed.
///
/// That makes the property STRUCTURAL — `best` is a maximum over probed
/// states, and the baseline is always one of them — rather than something a
/// test has to keep watch over. `SearchResult::baselineScore` publishes the
/// witness, `SearchResult::atLeastBaseline()` states the postcondition, and
/// `frontierSearch` asserts it before returning and prints it on the trace's
/// `summary` line as `>=baseline=<yes|no>`.
///
/// The assertion is the primary statement, not a fallback, and deliberately
/// so: the fallback is already the MECHANISM — keeping the best of a set that
/// contains the baseline — so a violation could only mean the score or the
/// probe is not a function of the state, and papering over that would hide
/// the real defect. But an assertion is compiled out of a release build,
/// which is precisely where a user is relying on the guarantee, so
/// tools/emitrust-cc checks `atLeastBaseline()` unconditionally as well: it
/// prints a loud internal error and then falls back to the unrestricted
/// import, the answer the user would have got without the flag. Loud first,
/// harmless second.
///
/// The complementary half of this guarantee lives in the importer: a
/// recovering import used to DROP a rejected record while still emitting
/// every field, local and parameter that named it, so the baseline could
/// score higher than the search with a crate that did not compile. It now
/// rejects those dependents too (`CImporter::importRecord`), which is what
/// makes "more items ported" mean the same thing on both sides of this
/// comparison.
///
/// FR-43's own text says "the Green∪Yellow closure of the ROOTS — `main` plus
/// every externally visible definition", and that is not what this does. The
/// reason is FR-41's coloring rule, which is a GLOBAL least fixpoint rather
/// than a reachability walk: an item is Green when it and its type closure
/// are admissible, whoever calls it. Intersecting that with root reachability
/// discards items that are perfectly portable and whose only referrer is Red
/// — and those are not a corner case. In the FR-46 corpus, `polygon`'s
/// `tu0_abs_int` is Green and is called only by the Red `perimeter_manhattan`,
/// and `shapes`' `tu2_isqrt` is Green and reached only through Red methods; a
/// root-closure root state admits NEITHER, taking the corpus from 19/31 ported
/// items to 16/31. Since raising the ported fraction is the entire purpose of
/// this search, the wider root state is the one implemented, and the roots are
/// kept for what they are genuinely good for: ordering. `searchRoots` computes
/// them, `rootDistances` measures each item's distance from them, and child
/// generation drops the item FURTHEST from a root first, so a repair costs the
/// project's entry points last.
///
///===--------------------------------------------------------------------===//
/// Expansion: what a probe teaches
///===--------------------------------------------------------------------===//
///
/// Probing a state yields a `ProbeOutcome`. Two kinds of fact come back:
///
///  - REJECTIONS, when the import succeeded. Each names an item the coloring
///    probe called admissible and the real import did not — precisely the
///    false Greens `ItemColoring.h` says it deliberately leaves behind
///    (pointer-to-pointer parameters, `void *` parameters, pointer returns,
///    pointer struct members and globals, variadics, `volatile`, scoped
///    enums, and every body-level rejection). Recovery has ALREADY handled
///    each of them, so a child that also excludes them explicitly reaches the
///    same module; such children exist to be scored and memoized away, and
///    they are why a project whose coloring is exact converges in ONE probe.
///  - A FAILURE, when the import did not survive at all. This is the case the
///    search exists for. The failure names a symbol ("... 'g' is referenced
///    but not defined in any translation unit") or at least carries a
///    location, and `blameCandidates` turns either into a deterministic,
///    ordered list of items whose removal could repair it: the REFERRERS of a
///    named-but-missing symbol, else the item enclosing the failure location.
///
/// A child state drops one candidate and RE-COLORS: the drop is fed to
/// `computeColoring` as a fresh inadmissibility seed on top of the original
/// probe's, so FR-41's asymmetry decides the fallout — dropping a FUNCTION
/// leaves its callers Yellow and admitted (they will call its stub), while
/// dropping a RECORD, ENUM or GLOBAL turns every dependent Red and removes it
/// too. The search never re-derives that rule; it calls FR-41's fixpoint with
/// different seeds, which is the only way the two can be guaranteed to agree.
///
///===--------------------------------------------------------------------===//
/// Score, bound, determinism
///===--------------------------------------------------------------------===//
///
/// `SearchScore` is lexicographic and is compared exactly as FR-43 specifies:
/// items emitted FOR REAL first, then the negated stub count, then the summed
/// representation cost. A state whose import failed scores below every state
/// whose import succeeded, at any counts.
///
/// The search is bounded by PROBES, not by states: `SearchOptions::maxNodes`
/// (default 8) is how many imports may be attempted, because the import is
/// the only expensive thing here and a bound in any other unit would not
/// bound the cost. It cannot loop: every child strictly removes at least one
/// item, so admitted sets strictly shrink along any path, and `signature()`
/// memoization stops the same set from ever being probed twice however many
/// ways the search reaches it.
///
/// Reproducibility is by construction, not by convention. Every container
/// this file publishes is sorted on content; the frontier is a total order
/// with no tie left for insertion order to break; blame enumerates a whole
/// FAILURE CLASS from the graph rather than chasing the one symbol a
/// diagnostic happened to name; and the probe is the only impure thing in
/// sight, injected as a `ProbeFn` so the whole search can be tested without an
/// importer, an MLIR context, or a filesystem.
///
/// One honest caveat, pinned by test/Project/search-determinism.c. The
/// importer's own whole-program diagnostic is order-dependent:
/// `finalizeProject` returns at the FIRST undefined symbol it meets, and which
/// one that is depends on the translation-unit order. That text is quoted
/// verbatim into the trace's `learn ... failure=` line, so THAT LINE can
/// differ between two permutations of the same project. Nothing else can: the
/// candidates, the probe order, the winner, the emitted crate and the FR-44
/// report are all identical, because the repair is derived from the graph's
/// complete set of undefined items and not from the symbol that was named.
///
///===--------------------------------------------------------------------===//
/// The representation dimension
///===--------------------------------------------------------------------===//
///
/// `enumerateRepresentations` returns exactly ONE candidate per item today —
/// the greedy Pass-A planners the importer already runs (`planOwners`,
/// `planCellSlices`, `planMallocPool`, ...), which is why this wave changes
/// no existing output: with one candidate, the "re-pick this item's
/// representation" branch of expansion generates nothing and the search is a
/// search over admitted sets alone. The dimension is plumbed anyway because
/// the multiple choice already EXISTS in the system: FR-39's container fat-op
/// split (array pool vs `Vec` vs `VecDeque`, W4.5) is a genuine per-item
/// decision made greedily today. When it becomes selectable, it plugs in by
/// returning several `RepresentationChoice`s here and reading the chosen one
/// in the probe — no re-plumbing of states, scores, memoization or the trace.
///
///===--------------------------------------------------------------------===//
/// Trace format
///===--------------------------------------------------------------------===//
///
/// `SearchResult::trace`, and `emitrust-cc --emit=search` /
/// `--search-trace=<path>`. One record per line; every field is a whole
/// `key=value` token separated by one space, so FileCheck patterns and
/// `grep ' outcome=failed '` work on whole tokens. The line kinds, in the
/// order they can appear:
///
/// \code
/// search items=<n> roots=<n> max-nodes=<n>
/// root <id> admitted=<n> excluded=<n>
/// baseline <id> admitted=<n> excluded=<n>
/// probe <id> outcome=<ok|failed> ported=<n> stubbed=<n> dropped=<n> \
///       rep-cost=<n>
/// learn <id> rejected=<symbol> as=<stub|drop> tag=<tag> new=<yes|no>
/// learn <id> failure=<symbol|-> reason=<verbatim text>
/// child <id> from=<id> drop=<symbol> cascade=<n> why=<learned|blamed>
/// prune <id> from=<id> drop=<symbol> why=<memoized|empty>
/// stop reason=<exhausted|node-budget>
/// best <id> ported=<n> stubbed=<n> rep-cost=<n>
/// admitted <symbol> rep=<name>
/// excluded <symbol> why=<red|dropped|cascade>
/// summary probes=<n> generated=<n> pruned=<n> improved=<yes|no> \
///         >=baseline=<yes|no>
/// \endcode
///
/// (each real line is unwrapped.) The `baseline` line appears exactly when the
/// admit-everything state differs from the root, i.e. when FR-41 ruled
/// something out; it is node 1 whenever it appears. `improved=yes` exactly
/// when the best state is not the root state — i.e. when a probe after the
/// first one won, which is the honest answer to "did the search earn its cost
/// on this project?". `>=baseline=yes` is the FR-50 postcondition, and it is
/// `yes` on every run by construction; a `no` would be a bug loud enough to
/// have already tripped the assertion in a debug build.
/// The `learn` lines are the answer to "why did it stop there": they are the
/// complete record of what each import attempt told the search, deduplicated
/// and sorted on content (the FR-42 ledger records a header's item once per
/// including translation unit, in declaration-walk order, and neither the
/// repetition nor that order is a fact about the project). `new=yes` marks
/// the ones the state ADMITTED and the import still refused — the false
/// Greens, the only rejections the search can act on. `new=no` covers the
/// items the state had already excluded (which reject with the search's own
/// synthetic reason, so reporting them as discoveries would be circular) and
/// the rejections of things the graph does not model at all, such as C++
/// member functions.
///
/// An `excluded` line's `why` separates three different fixes: `red` — FR-41
/// ruled the item out before any import, so support the construct its blame
/// chain names; `dropped` — the search removed it because a probe rejected
/// it, so support what that probe rejected; `cascade` — nothing is wrong with
/// the item at all, it merely depends on a `dropped` one and FR-41's type
/// poisoning took it down with it.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_PROJECT_FRONTIERSEARCH_H
#define EMITRUST_PROJECT_FRONTIERSEARCH_H

#include "EmitRust/Project/ItemColoring.h"
#include "EmitRust/Project/ItemGraph.h"

#include "mlir/Support/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mlir {
namespace emitrust {

//===----------------------------------------------------------------------===//
// The representation dimension
//===----------------------------------------------------------------------===//

/// One way an admitted item can be emitted.
///
/// Exactly one of these exists per item today; see the file comment for why
/// the dimension is here anyway and where FR-39's container split plugs in.
struct RepresentationChoice {
  /// A stable identifier, printed in the trace's `rep=` token. `default` is
  /// the greedy Pass-A planning the importer already does.
  std::string name;
  /// The cost added to `SearchScore::repCost` when this choice is taken.
  /// Zero for `default`, so today's score never depends on this dimension.
  unsigned cost = 0;
};

/// The representation candidates available for `item`, best-first.
///
/// Total: every item has at least one candidate, so a state always has a
/// representation for every symbol it admits.
///
/// \param item the graph node to enumerate choices for.
/// \returns the candidates; index 0 is the greedy default.
std::vector<RepresentationChoice> enumerateRepresentations(const ItemNode &item);

//===----------------------------------------------------------------------===//
// States
//===----------------------------------------------------------------------===//

/// A candidate partial port: which items to admit, and how to emit each.
struct SearchState {
  /// The admitted items' symbols, sorted and unique. Every entry is an
  /// FR-40 node key.
  std::vector<std::string> admitted;
  /// The index into `enumerateRepresentations` chosen for each entry of
  /// `admitted`, parallel to it. All zero today.
  std::vector<unsigned> representation;

  /// True when `symbol` is admitted.
  bool admits(llvm::StringRef symbol) const;

  /// The memoization key: a total, content-only encoding of the whole state
  /// (both vectors), so two states with the same admitted set but different
  /// representations are distinct entries.
  std::string signature() const;

  /// The summed cost of the chosen representations.
  ///
  /// \param graph the graph the symbols come from, for the candidate lists.
  unsigned representationCost(const ItemGraph &graph) const;
};

/// The items the search starts from: `main` (imported as `c_main`) plus every
/// externally visible DEFINITION of the project, sorted.
///
/// Not used to build the root state — see the file comment — but to order
/// repairs: an item far from every root is dropped before one near a root.
///
/// \param graph the project's item graph.
/// \returns the root symbols, sorted.
std::vector<std::string> searchRoots(const ItemGraph &graph);

/// Each item's distance from the nearest root, following dependency edges
/// forward (a root is 0, a root's direct dependency 1, ...). An item no root
/// reaches gets `UINT_MAX`, which sorts it first among drop candidates: it is
/// the least load-bearing thing in the project.
///
/// \param graph the project's item graph.
/// \param roots the result of `searchRoots`.
/// \returns distance by symbol, one entry per graph node.
std::map<std::string, unsigned> rootDistances(const ItemGraph &graph,
                                              llvm::ArrayRef<std::string> roots);

/// The root state: every Green or Yellow item of `coloring`, each at its
/// default representation.
///
/// \param graph the project's item graph.
/// \param coloring the FR-41 coloring of that graph.
/// \returns the root state.
SearchState rootState(const ItemGraph &graph, const ItemColoring &coloring);

/// The BASELINE state: every item of `graph`, at its default representation.
///
/// Probing it excludes nothing, so it is bit for bit the import a plain
/// `--emit=crate --incremental` run performs — which is what makes it the
/// witness for FR-50's "the search is never worse than not searching". It
/// admits Red items on purpose: whether the coloring was right about them is
/// precisely the question a probe answers better than an approximation does.
///
/// \param graph the project's item graph.
/// \returns the baseline state.
SearchState baselineState(const ItemGraph &graph);

//===----------------------------------------------------------------------===//
// Probes
//===----------------------------------------------------------------------===//

/// One rejection a probe's import reported.
struct LearnedRejection {
  /// The rejected item's symbol, as the FR-42 ledger recorded it.
  std::string symbol;
  /// True when a signature-preserving stub replaced it.
  bool stubbed = false;
  /// The FR-42 blocker tag.
  std::string blockerTag;
};

/// What one import attempt told the search.
struct ProbeOutcome {
  /// False when the import (or the lowering that must follow it) failed
  /// outright, in which case the state produces no crate at all.
  bool imported = false;
  /// Items emitted as real Rust items. Zero when `imported` is false.
  unsigned ported = 0;
  /// Items emitted as `unimplemented!()` stubs.
  unsigned stubbed = 0;
  /// Items not emitted at all.
  unsigned dropped = 0;
  /// The rejections, in declaration-walk order; empty when `imported` is
  /// false and the failure was not attributable to a walk step.
  std::vector<LearnedRejection> rejections;
  /// The verbatim first error, when `imported` is false; empty otherwise.
  std::string failure;
  /// The symbol that error named, when it named one — the missing definition
  /// of a "referenced but not defined" failure. Empty when the failure names
  /// no symbol.
  std::string failureSymbol;
  /// The file of the failure's location, empty when it had none.
  std::string failureFile;
  /// The 1-based line of `failureFile`; 0 when unknown.
  unsigned failureLine = 0;
};

/// Runs one import attempt for a state. The single impure input of the
/// search, injected so the whole algorithm is testable without an importer.
using ProbeFn = std::function<ProbeOutcome(const SearchState &)>;

/// A state's score, compared lexicographically by `isBetterThan`.
struct SearchScore {
  /// False for a state whose import failed; such a state loses to every
  /// state whose import succeeded, whatever the counts.
  bool imported = false;
  /// Items emitted for real — the primary key, maximized.
  unsigned ported = 0;
  /// Stubs — the secondary key, minimized (FR-43's "negated stub count").
  unsigned stubbed = 0;
  /// Summed representation cost — the tertiary key, minimized.
  unsigned repCost = 0;

  /// Strict lexicographic betterness: `imported`, then `ported` ascending,
  /// then `stubbed` descending, then `repCost` descending.
  bool isBetterThan(const SearchScore &other) const;
};

//===----------------------------------------------------------------------===//
// The search
//===----------------------------------------------------------------------===//

/// Knobs. Defaults are FR-43's.
struct SearchOptions {
  /// How many IMPORT ATTEMPTS the search may make, including the root's.
  /// The import dominates the cost, so this is the only bound that bounds
  /// anything. Zero is treated as one: a search that probes nothing has no
  /// state to return.
  unsigned maxNodes = 8;
};

/// One explored state, in probe order.
struct SearchNode {
  /// The node's id: its index in `SearchResult::nodes`, so 0 is the root.
  unsigned id = 0;
  /// The id of the node this was derived from; `id` itself for the root.
  unsigned parent = 0;
  /// The item dropped relative to `parent`; empty for the root.
  std::string drop;
  /// Whether `drop` came from a rejection the probe reported (`learned`) or
  /// from blame attribution on a failed import (`blamed`); `baseline` for the
  /// mandatory admit-everything probe, which drops nothing; empty for the
  /// root.
  std::string why;
  /// The state itself.
  SearchState state;
  /// What its probe returned.
  ProbeOutcome outcome;
  /// Its score.
  SearchScore score;
};

/// The whole search: a deterministic value, fully sorted.
struct SearchResult {
  /// The state that won.
  SearchState best;
  /// Its score.
  SearchScore bestScore;
  /// Its node id.
  unsigned bestNode = 0;
  /// Every probed state, in probe order; `nodes[0]` is the root.
  std::vector<SearchNode> nodes;
  /// States generated (probed or pruned).
  unsigned generated = 0;
  /// States generated but never probed, because their signature had been
  /// seen or they admitted nothing new.
  unsigned pruned = 0;
  /// Why the loop ended: `exhausted` (no candidate left) or `node-budget`.
  std::string stopReason;
  /// The score of the BASELINE state — the admit-everything import a plain
  /// `--incremental` run performs. When the coloring rules nothing out the
  /// root already admits everything, the two states are one state, and this
  /// is the root's own score.
  SearchScore baselineScore;
  /// The baseline's node id: 1 when it was probed on its own, 0 when it
  /// coincided with the root.
  unsigned baselineNode = 0;
  /// The rendered trace, in the format documented at the top of this file.
  std::string trace;

  /// True when the winner is not the root — i.e. when some probe after the
  /// first one won. That includes the FR-50 baseline: if the admit-everything
  /// import beats the coloring's root state, the search DID improve on its
  /// own starting point, and it also just proved FR-41 called something Red
  /// that imports.
  bool improved() const { return bestNode != 0; }

  /// FR-50's postcondition: the winner is no worse than not searching at all.
  ///
  /// True by construction — `best` is the maximum over a probed set that
  /// always contains the baseline — so a caller checks this to catch a bug,
  /// not to choose a code path.
  bool atLeastBaseline() const { return !baselineScore.isBetterThan(bestScore); }
};

/// The functional core: searches for the best admissible subset of `graph`.
///
/// Pure except for `probe`, and deterministic given a deterministic `probe`:
/// it touches no filesystem, no clock and no global state, and every choice
/// it makes is a total order on content.
///
/// \param graph the project's item graph (FR-40).
/// \param admissibility the coloring probe's seeds (FR-41), re-used with
///        extra seeds to re-color after each drop.
/// \param options the bound.
/// \param probe the import attempt.
/// \returns the search result, always with at least the root node probed.
SearchResult frontierSearch(const ItemGraph &graph,
                            const ItemAdmissibility &admissibility,
                            const SearchOptions &options, const ProbeFn &probe);

//===----------------------------------------------------------------------===//
// Inputs
//===----------------------------------------------------------------------===//

/// The three analyses a search needs, computed from ONE parse of the project.
///
/// Parsing once matters twice over: the import attempts already re-parse the
/// project per probe, and a second analytical parse could in principle
/// disagree with the first about what the project even contains.
struct SearchInputs {
  /// The FR-40 item graph.
  ItemGraph graph;
  /// The FR-41 admissibility seeds.
  ItemAdmissibility admissibility;
  /// The FR-41 coloring implied by the two above.
  ItemColoring coloring;
};

/// Parses `paths` (through the same shell `importCProject` and
/// `buildItemGraph` use, with the same FR-45 `compile_commands.json`
/// semantics) and computes all three analyses.
///
/// \param paths the source files, or empty with a database to mean "all of
///        it".
/// \param extraClangArgs additional clang arguments.
/// \param compilationDatabasePath a `compile_commands.json` or its
///        directory; empty for none.
/// \param error receives the reason when the database cannot be loaded.
/// \returns the inputs, or failure (parse diagnostics go to stderr as usual).
FailureOr<SearchInputs> buildSearchInputs(llvm::ArrayRef<std::string> paths,
                                          llvm::ArrayRef<std::string> extraClangArgs,
                                          llvm::StringRef compilationDatabasePath,
                                          std::string &error);

/// The complement of `state` over `graph`: the symbols to hand to
/// `ImportOptions::excludedItems` when probing it.
///
/// \param graph the project's item graph.
/// \param state the state to probe.
/// \returns the excluded symbols.
std::set<std::string> excludedItemsFor(const ItemGraph &graph,
                                       const SearchState &state);

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_PROJECT_FRONTIERSEARCH_H

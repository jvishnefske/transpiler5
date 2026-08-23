//===- ProgressReport.h - Pure per-item porting report rendering -*- C++ -*-==//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// FR-44: the functional core of `emitrust-cc --emit=crate --incremental`.
/// Pure functions that JOIN four inputs — the FR-40 whole-project item graph
/// (the denominator: every item the project HAS), the FR-42 rejection ledger
/// (the numerator's complement: every item the importer could not translate),
/// the symbol table of the module actually emitted (the evidence that a
/// surviving item really did become Rust), and the FR-41 coloring (FR-49: the
/// blame chain that says which construct is ACTUALLY to blame) — into one
/// per-item report, and render it as `PORTING.md` (human) and
/// `emitrust-progress.json` (machine).
///
/// Nothing here touches the filesystem, the clock, or any other side effect:
/// the imperative shell in emitrust-cc.cpp gathers the four inputs and
/// writes the two returned strings out, exactly as it does for CrateEmitter's
/// `Cargo.toml` and crate root.
///
/// Nothing in this file depends on the crate's SHAPE. FR-51 made a project
/// whose `main` is dropped emit a LIBRARY crate instead of failing outright,
/// which is the case that turned this report from unreachable into the only
/// account of what happened -- but the report's inputs are the item graph,
/// the ledger, the emitted symbol table and the coloring, none of which knows
/// whether the crate root ended up being `main.rs` or `lib.rs`.
///
//===----------------------------------------------------------------------===//
//
/// # FR-49: root-cause attribution
///
/// A rejection names the construct the importer TRIPPED OVER, which is very
/// often not the construct to fix. `shapes` is the worked example: thirteen
/// of its rejections read `unsupported: method of an unimported class`, one
/// per member function of four classes — but every one of those methods is
/// unimportable only because `Shape` has a user-declared destructor and
/// `Rect`/`Circle`/`RightTriangle` have base classes. Ranking the backlog by
/// the reported diagnostic points a reader at thirteen SYMPTOMS; ranking it
/// by root cause points at the four constructs that would actually unblock
/// them.
///
/// The attribution is a JOIN, not a new analysis. FR-41's `ItemColoring`
/// already computes, for every non-Green item, the immediate poisoner
/// (`via`), the edge that carried the poison, the whole `chain` to the
/// inadmissible item at its root, and that root's `construct` tag. This file
/// only looks it up and carries it into both artifacts:
///
///  - `ProgressItem::rootBlockerTag` is the coloring's `construct` — the ONE
///    construct whose support would unblock the item.
///  - `ProgressItem::blameChain` is the audit trail for that claim, so a
///    reader can check the attribution instead of trusting it.
///  - `ProgressReport::rootBlockerRanking` ranks by root; the pre-existing
///    `blockerRanking` still ranks by the reported diagnostic. BOTH are
///    published, in both artifacts. Nothing is thrown away.
///
/// ## Two vocabularies, deliberately not merged
///
/// A root tag comes from the FR-41 probe's construct vocabulary
/// (`base-class`, `destructor`, `copy-move-constructor`, `template`, ...); a direct
/// tag comes from FR-42's `classifyBlocker` (`cxx-inheritance`,
/// `cxx-destructor`, `dynamic-memory`, ...). They are different vocabularies
/// with different provenance — one is a syntactic screen over the AST, the
/// other a heuristic over diagnostic text shared with the RealWorld survey —
/// and translating between them would invent an equivalence neither side
/// guarantees. The artifacts therefore label which is which and keep both.
///
/// ## The fallback, and why it is not "unknown"
///
/// An item with no coloring entry, or one the coloring calls Green (the
/// probe deliberately UNDER-approximates: it leaves Green everything it is
/// unsure about, so most C rejections have no chain at all), is credited to
/// its OWN direct blocker tag. That is the truthful answer — with no chain,
/// the item is its own root — and it means the root ranking degrades exactly
/// to the direct ranking on a project the coloring has nothing to say about,
/// rather than collapsing into an `unknown` bucket.
///
/// ## Off-graph items: attribution through the enclosing class
///
/// C++ member functions are not item-graph nodes (`ItemGraph.h` documents
/// why), so they have no color and no chain of their own — and all thirteen
/// `shapes` symptoms are member functions. They are attributed to their
/// enclosing CLASS, which IS a node, IS colored, and DOES carry a chain: the
/// method's root is its class's root, and its blame chain is the method
/// prepended to the class's. The join key is `RejectedItem::ownerSymbol`,
/// recorded by the importer at rejection time, NOT recovered from the
/// symbol: nothing in `area_x100` says `Rect`, and three sibling classes
/// each define one.
///
/// Three limits of that mapping, stated rather than papered over:
///
///  1. A CONSTRUCTOR is spelled like its class (`Rect::Rect` is `Rect`), so
///     its rejection collides with the class's own node key and is absorbed
///     by the class's graph item by the join below — it never reaches
///     `offGraphItems` at all. The absorbed row's root is still the class's
///     root, which is the constructor's root too, so the ATTRIBUTION stays
///     correct; what is conflated is the item IDENTITY (one row covers the
///     class and its constructors). This predates FR-49 and is unchanged by
///     it.
///  2. A class the graph does not model — block-scope, or declared inside a
///     namespace, or with no definition in the translation unit — yields an
///     empty `ownerSymbol`, and its methods fall back to their own direct
///     tag rather than being attributed to a node that is not there.
///  3. The class's root explains why the method could not be imported IN
///     THIS RUN. A method may independently contain constructs of its own
///     that would block it even after its class is supported; the chain does
///     not claim otherwise, and such a method simply reappears with a new
///     blocker once its class is fixed.
///
//===----------------------------------------------------------------------===//
//
/// # `emitrust-progress.json` schema (version `emitrust-progress/1`)
///
/// One JSON object, printed with two-space indentation and a trailing
/// newline. Every array is sorted by a total order on content only, so two
/// runs over the same sources produce byte-identical output and two runs over
/// DIFFERENT revisions diff cleanly line by line — which is the whole point:
/// a later per-item ratchet consumes this file, keys items by `symbol`, and
/// reports the status transitions between two runs.
///
/// FR-49's additions are STRICTLY ADDITIVE — a new `root_blockers` array and
/// three new per-item fields — so the version stays `emitrust-progress/1`:
/// every key an `emitrust-progress/1` consumer reads is still present, still
/// spelled the same, and still means the same thing. In particular
/// `blockers` keeps its FR-44 meaning (the DIRECT, as-reported tally); the
/// root-cause tally is the new key beside it, and the reordering to
/// root-first happens only in `PORTING.md`, which has no consumers but
/// people. Bumping the version would have forced `run_realworld.py`'s
/// ratchet to change for a report it does not read a single new field of.
///
/// Field by field (the JSON itself carries no comments):
///  - `schema` — format id; bumped only on an INCOMPATIBLE change.
///  - `crate` — the sanitized cargo package name.
///  - `denominator_source` — `item-graph` or `ledger-only`; see below.
///  - `totals.graph_items` — graph items that CAN be ported (see `declared`).
///  - `totals.ported` / `stubbed` / `dropped` / `missing` / `declared` — the
///    per-status counts over `items`.
///  - `totals.off_graph_rejected` — rejected items the graph does not model.
///  - `totals.ported_permille` — `1000 * ported / graph_items`, truncated; 0
///    when `graph_items` is 0.
///  - `blockers` — the DIRECT, as-reported tally: `{ "tag", "count" }`
///    objects sorted by count descending then tag ascending.
///  - `root_blockers` — FR-49: the same items tallied by ROOT cause, same
///    object shape and same order.
///  - `items` — one object per graph item; ordering below.
///  - `off_graph_items` — the same object shape for rejected items the graph
///    does not model; `kind` and `linkage` are empty and `tu` is 0.
///
/// Each item object carries:
///  - `symbol`, `kind` (`function`/`record`/`enum`/`global`), `status`
///    (`ported`/`stubbed`/`dropped`/`missing`/`declared`), `color`
///    (`green`/`yellow`/`red`/`orange`/`grey`), `linkage`
///    (`extern`/`intern`), `tu`, `file`, `line`, `column`.
///  - `blocker` — the FR-42 tag of the diagnostic actually raised; `""` when
///    nothing was rejected.
///  - `diagnostic` — that diagnostic, verbatim.
///  - `root_blocker` — FR-49: the construct at the end of this item's blame
///    chain, in the FR-41 probe vocabulary when a chain was available and in
///    the FR-42 vocabulary when it was not; `""` when nothing was rejected.
///  - `blame_chain` — FR-49: the audit trail for `root_blocker`, an array of
///    symbols from this item to the root, `[]` when nothing was rejected and
///    `[symbol]` when the item is its own root.
///  - `attributed_via` — FR-49: the graph node whose chain was borrowed, for
///    an off-graph item attributed through its enclosing class; `""`
///    otherwise.
///
/// ## What the denominator is, and what it deliberately is not
///
/// `items` is the FR-40 item graph, which is built from the clang ASTs
/// independently of whether the import succeeds — so it is a REAL inventory
/// of the project, not a list of whatever happened to survive. `graph_items`
/// counts every graph item with a definition in the project; a node that is
/// only a prototype or an `extern` declaration is reported with status
/// `declared` and excluded from both numerator and denominator, because there
/// is nothing there to port.
///
/// `ItemGraph.h` documents exactly which items it does NOT model: C++ member
/// functions, anonymous and block-scope records, and records whose emitted
/// name depends on accumulated importer state. Those can still be REJECTED,
/// and a rejection of one carries a symbol that is not a node key. Rather
/// than inflate the denominator with a partial, one-sided count of them (they
/// are invisible when they succeed, so counting them when they fail would
/// make a project look worse the more of it ported), they are reported
/// separately in `off_graph_items` and tallied in `totals.off_graph_rejected`.
/// The headline fraction is therefore over graph items only, and IS
/// comparable across projects and across revisions.
///
/// `denominator_source` is `ledger-only` when the graph could not be built at
/// all (a parse failure in the second, analytical parse). In that mode
/// `items` is empty, every rejection lands in `off_graph_items`, and
/// `graph_items`/`ported_permille` are zero — a consumer must not read a
/// `ledger-only` report as "0% ported".
///
/// ## Ordering
///
/// `items` is sorted by (status rank, blocker tag, symbol), where the status
/// rank is dropped < stubbed < missing < ported < declared: the unported work
/// comes first and clusters by blocker, which is the order a person reads the
/// table in. `off_graph_items` is sorted by (symbol, file, line, column) —
/// its symbols are not unique (three classes may each define `area_x100`, and
/// a constructor is spelled like its class), so the location is part of the
/// key.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_TOOLS_EMITRUST_CC_PROGRESSREPORT_H
#define EMITRUST_TOOLS_EMITRUST_CC_PROGRESSREPORT_H

#include "EmitRust/ImportC.h"
#include "EmitRust/Project/ItemColoring.h"
#include "EmitRust/Project/ItemGraph.h"

#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

#include <string>
#include <vector>

namespace emitrustcc {

/// What became of one program item. The five values partition every item the
/// report describes; `itemStatusName` gives the stable spelling used in both
/// artifacts and in the checked-in `expected-items.txt` ratchet.
enum class ItemStatus {
  /// A real Rust item was emitted for it: it is in the module's symbol table
  /// and the importer never rejected it.
  Ported,
  /// The importer rejected it but its signature still mapped, so an
  /// `unimplemented!()` stub carrying that signature stands in its place.
  Stubbed,
  /// The importer rejected it and emitted nothing at all.
  Dropped,
  /// Never rejected, yet absent from the emitted module. Not expected in
  /// practice; reported rather than silently folded into `dropped` so that a
  /// gap between the graph's naming and the importer's is visible instead of
  /// quietly shrinking the numerator.
  Missing,
  /// The project only DECLARES it (a function prototype, an `extern`
  /// global); there is no definition to port. Excluded from the fraction.
  Declared,
};

/// The stable lowercase spelling of `status`, used in the JSON, in
/// `PORTING.md`, and in the `expected-items.txt` ratchet.
llvm::StringRef itemStatusName(ItemStatus status);

/// The report's color for `status`: `green` (ported), `yellow` (stubbed),
/// `red` (dropped), `orange` (missing), `grey` (declared). Kept as data
/// rather than markup so the two renderers agree and a third consumer can
/// pick its own presentation.
llvm::StringRef itemStatusColor(ItemStatus status);

/// The rank that orders `ProgressReport::items`: dropped(0) < stubbed(1) <
/// missing(2) < ported(3) < declared(4), so unported work sorts first.
unsigned itemStatusRank(ItemStatus status);

/// One row of the report: an item, what became of it, and where it is.
struct ProgressItem {
  /// The emitted Rust item name for a graph item; for an off-graph item, the
  /// symbol the rejection carried (a bare C++ method spelling, say), which is
  /// NOT unique on its own.
  std::string symbol;
  /// `function`, `record`, `enum`, or `global`; empty for an off-graph item,
  /// whose kind the ledger does not record.
  std::string kind;
  /// What became of it.
  ItemStatus status;
  /// The FR-42 blocker tag of the diagnostic this item actually raised — the
  /// SYMPTOM. Empty unless the item was rejected, with one FR-115 exception:
  /// a `missing` item (a definition node the import never visited) carries
  /// the synthetic tag `unreached-by-import`, an honest attribution of the
  /// non-visit rather than a diagnostic tag. (Since FR-126 the template-walk
  /// shape of that bucket is ledgered as `template-sibling-not-reached`
  /// instead, a located row naming the sibling that aborted the walk.)
  std::string blockerTag;
  /// The verbatim importer diagnostic; empty unless the item was rejected
  /// (in particular, empty for `unreached-by-import` — nothing was ever
  /// diagnosed).
  std::string diagnostic;
  /// FR-49: the construct at the end of this item's blame chain — the one
  /// construct whose support would unblock it. Taken from the FR-41
  /// coloring's `construct` when a chain was available (and then in the
  /// PROBE's vocabulary: `base-class`, `destructor`, ...); otherwise equal to
  /// `blockerTag`, because an item with no chain is its own root. Since
  /// FR-115 this is no longer empty exactly when `blockerTag` is: an item
  /// with no ledger row of its own still publishes the coloring's root when
  /// the poison chain reached it, and a `missing` item self-roots as
  /// `unreached-by-import`. Empty only when there is neither a tag nor a
  /// color.
  std::string rootBlockerTag;
  /// FR-49: the audit trail for `rootBlockerTag` — symbols from this item to
  /// the root. Empty when `rootBlockerTag` is; `{symbol}` when the item
  /// is its own root; for an off-graph item, `symbol` followed by its
  /// enclosing class's own chain.
  std::vector<std::string> blameChain;
  /// FR-49: the graph node whose chain was borrowed, set for an off-graph
  /// item attributed through its enclosing class; since FR-126 also the
  /// rejected TYPE (or failed template sibling) a cascade's chain was
  /// resolved through when the coloring had no chain of its own. Empty
  /// otherwise, including for every graph item that used its own chain.
  std::string attributedVia;
  /// Where to look: the item's declaration for a ported item, the rejection's
  /// own location for a rejected one (so it points at the offending
  /// construct, not at the enclosing declaration).
  std::string file;
  /// 1-based line of `file`, or 0 when the location is not a file location.
  unsigned line = 0;
  /// 1-based column of `file`, or 0 as above.
  unsigned column = 0;
  /// The translation unit the item is attributed to; 0 for an off-graph item.
  unsigned tuIndex = 0;
  /// `extern` or `intern`; empty for an off-graph item.
  std::string linkage;
};

/// The whole per-item report: a deterministic value, fully sorted, that both
/// renderers below are pure functions of.
struct ProgressReport {
  /// The sanitized cargo package name the crate was emitted under.
  std::string crateName;
  /// False when the item graph could not be built, in which case `items` is
  /// empty and the fraction is not meaningful (`denominator_source` is
  /// `ledger-only`).
  bool haveItemGraph = true;
  /// The graph items, ordered by (status rank, blocker tag, symbol).
  std::vector<ProgressItem> items;
  /// Rejected items the graph does not model, ordered by (symbol, file, line,
  /// column). Never `Ported` — an item is only here because it was rejected.
  std::vector<ProgressItem> offGraphItems;

  /// The number of `items` with `status`.
  unsigned count(ItemStatus status) const;
  /// Items that could be ported: everything in `items` except `Declared`.
  unsigned portableItems() const;
  /// `1000 * ported / portableItems()`, truncated; 0 when there is nothing to
  /// port. Integer permille rather than a float so the artifacts contain no
  /// locale- or rounding-dependent text.
  unsigned portedPermille() const;
  /// DIRECT blocker tags over `items` and `offGraphItems` together, ordered
  /// by count descending then tag ascending — what each item REPORTED.
  std::vector<std::pair<std::string, unsigned>> blockerRanking() const;
  /// FR-49: the same items tallied by `rootBlockerTag` instead, same order —
  /// the ranked backlog of constructs actually worth fixing. Both tallies
  /// have the same total (every rejected item has exactly one of each), but
  /// they concentrate differently: on `shapes` the direct tally is thirteen
  /// cascade symptoms and one exclusion tag, while the root tally names the
  /// four classes' two constructs.
  std::vector<std::pair<std::string, unsigned>> rootBlockerRanking() const;
};

/// The names of every top-level symbol operation in `module`.
///
/// This is the evidence that an item really was emitted: `emitrust.func`,
/// `emitrust.struct_def`, `emitrust.enum_def` and `emitrust.global` all carry
/// the `Symbol` trait, and FR-40 guarantees a graph node's key IS the emitted
/// name, so membership in this set is a direct answer to "did this item
/// become Rust?".
///
/// \param module the module about to be rendered into the crate.
/// \returns the set of top-level symbol names.
llvm::StringSet<> collectEmittedSymbols(mlir::ModuleOp module);

/// Joins the item graph, the rejection ledger, the emitted symbol table, and
/// (FR-49) the item coloring into a report.
///
/// Rejections are first collapsed: the same declaration is rejected once per
/// translation unit that parses the header holding it, so entries identical
/// in (symbol, file, line, column) are one item. A graph item then absorbs
/// EVERY remaining rejection carrying its symbol (a definition and its
/// prototype reject at different locations); it is `Stubbed` if any of them
/// stubbed, else `Dropped`, and its reported location and diagnostic come
/// from the first such rejection in declaration-walk order.
///
/// FR-49: `coloring`, when given, supplies the root-cause attribution. A
/// graph item takes its own `construct` and `chain`; an off-graph item takes
/// its enclosing class's, found through `RejectedItem::ownerSymbol`. An item
/// the coloring says nothing about keeps its direct tag as its root, so
/// passing null degrades the report to FR-44's behavior with `root_blocker`
/// simply mirroring `blocker` — never to a missing or `unknown` field.
///
/// \param crateName the sanitized cargo package name.
/// \param graph the FR-40 item graph, or null when it could not be built.
/// \param coloring the FR-41 coloring of that same graph, or null when it is
///        unavailable; ignored unless `graph` is non-null.
/// \param rejected the FR-42 ledger's items, in declaration-walk order.
/// \param emittedSymbols the result of `collectEmittedSymbols`.
/// \returns the fully sorted report.
ProgressReport
buildProgressReport(llvm::StringRef crateName,
                    const mlir::emitrust::ItemGraph *graph,
                    const mlir::emitrust::ItemColoring *coloring,
                    llvm::ArrayRef<mlir::emitrust::RejectedItem> rejected,
                    const llvm::StringSet<> &emittedSymbols);

/// Renders `PORTING.md`: a headline fraction, the ROOT blocker table, the
/// direct blocker table beneath it, the per-item table, and the off-graph
/// table when it is nonempty.
///
/// FR-49 puts the root table FIRST because it is the work queue — the
/// constructs whose support would unblock the most items — and keeps the
/// direct table right under it, labelled as the symptoms, so the two are
/// read together rather than one silently replacing the other.
///
/// \param report the report to render.
/// \returns the complete markdown text.
std::string renderPortingMarkdown(const ProgressReport &report);

/// Renders `emitrust-progress.json` in the schema documented at the top of
/// this file.
///
/// \param report the report to render.
/// \returns the complete JSON text, newline-terminated.
std::string renderProgressJson(const ProgressReport &report);

} // namespace emitrustcc

#endif // EMITRUST_TOOLS_EMITRUST_CC_PROGRESSREPORT_H

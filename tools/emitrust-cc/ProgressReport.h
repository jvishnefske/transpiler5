//===- ProgressReport.h - Pure per-item porting report rendering -*- C++ -*-==//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// FR-44: the functional core of `emitrust-cc --emit=crate --incremental`.
/// Pure functions that JOIN three inputs — the FR-40 whole-project item graph
/// (the denominator: every item the project HAS), the FR-42 rejection ledger
/// (the numerator's complement: every item the importer could not translate),
/// and the symbol table of the module actually emitted (the evidence that a
/// surviving item really did become Rust) — into one per-item report, and
/// render it as `PORTING.md` (human) and `emitrust-progress.json` (machine).
///
/// Nothing here touches the filesystem, the clock, or any other side effect:
/// the imperative shell in emitrust-cc.cpp gathers the three inputs and
/// writes the two returned strings out, exactly as it does for CrateEmitter's
/// `Cargo.toml`/`src/main.rs`.
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
/// \code
/// {
///   "schema": "emitrust-progress/1",   // format id; bump on any incompatible
///   change "crate": "polygon",                // sanitized cargo package name
///   "denominator_source": "item-graph",// "item-graph" | "ledger-only" (see
///   below) "totals": {
///     "graph_items":  8,   // items in the graph that CAN be ported (see
///     `declared`) "ported":       2,   // emitted as a real Rust item
///     "stubbed":      3,   // emitted as an unimplemented!() stub
///     "dropped":      3,   // not emitted at all
///     "missing":      0,   // in the graph, never rejected, yet absent from
///     the module "declared":     0,   // prototype/extern-only; NOT counted in
///     graph_items "off_graph_rejected": 0,  // rejected items the graph does
///     not model "ported_permille": 250    // 1000 * ported / graph_items,
///     truncated; 0 when graph_items == 0
///   },
///   "blockers": [                      // sorted by count desc, then tag
///     { "tag": "cxx-references", "count": 5 }
///   ],
///   "items": [                         // the graph items; see ordering below
///     {
///       "symbol": "shoelace_twice",
///       "kind": "function",            // function | record | enum | global
///       "status": "dropped",           // ported | stubbed | dropped | missing
///       | declared "color": "red",                // green | yellow | red |
///       orange | grey "linkage": "extern",           // extern | intern "tu":
///       0,                       // translation-unit index "file":
///       "/abs/path/geom.cpp", "line": 5, "column": 52, "blocker":
///       "cxx-references",   // "" when nothing was rejected "diagnostic":
///       "unsupported: reference types are not yet supported"
///     }
///   ],
///   "off_graph_items": [ ... same object shape, "kind" is "" ... ]
/// }
/// \endcode
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
  /// The FR-42 blocker tag; empty unless the item was rejected.
  std::string blockerTag;
  /// The verbatim importer diagnostic; empty unless the item was rejected.
  std::string diagnostic;
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
  /// Blocker tags over `items` and `offGraphItems` together, ordered by count
  /// descending then tag ascending — the ranked backlog.
  std::vector<std::pair<std::string, unsigned>> blockerRanking() const;
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

/// Joins the item graph, the rejection ledger, and the emitted symbol table
/// into a report.
///
/// Rejections are first collapsed: the same declaration is rejected once per
/// translation unit that parses the header holding it, so entries identical
/// in (symbol, file, line, column) are one item. A graph item then absorbs
/// EVERY remaining rejection carrying its symbol (a definition and its
/// prototype reject at different locations); it is `Stubbed` if any of them
/// stubbed, else `Dropped`, and its reported location and diagnostic come
/// from the first such rejection in declaration-walk order.
///
/// \param crateName the sanitized cargo package name.
/// \param graph the FR-40 item graph, or null when it could not be built.
/// \param rejected the FR-42 ledger's items, in declaration-walk order.
/// \param emittedSymbols the result of `collectEmittedSymbols`.
/// \returns the fully sorted report.
ProgressReport
buildProgressReport(llvm::StringRef crateName,
                    const mlir::emitrust::ItemGraph *graph,
                    llvm::ArrayRef<mlir::emitrust::RejectedItem> rejected,
                    const llvm::StringSet<> &emittedSymbols);

/// Renders `PORTING.md`: a headline fraction, a ranked blocker table, the
/// per-item table, and the off-graph table when it is nonempty.
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

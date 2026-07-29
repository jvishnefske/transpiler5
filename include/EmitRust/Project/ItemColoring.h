//===- ItemColoring.h - three-color lattice over the item graph -*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// FR-41: a three-color lattice over FR-40's program item graph, computed by
/// least fixpoint, answering "which items of this project can be emitted, and
/// which one is to blame when an item cannot?".
///
/// What this is for. FR-40 says what depends on what; FR-42 says what the
/// importer actually rejected on one concrete run. Neither answers the
/// question a whole-project port planner asks FIRST: given a project the
/// importer cannot yet translate WHOLE, which subset of it is translatable,
/// and what is the minimal set of constructs standing between here and more?
/// That question has to be answerable BEFORE any import (a project may fail
/// to import at all), so like the item graph this is a pure analysis over the
/// same clang ASTs, sharing the importer's clang-driving shell and its symbol
/// naming but none of its IR-building state.
///
///===--------------------------------------------------------------------===//
/// The three colors
///===--------------------------------------------------------------------===//
///
///  - GREEN: the item and its whole dependency closure are inside the
///    supported subset. It emits, and everything it needs emits.
///  - YELLOW: the item itself is admissible and its TYPE closure is clean,
///    but at least one function it calls is Red. It is still emitted, and it
///    compiles, because the Red callee is replaced by a signature-preserving
///    stub (FR-42's recovery does exactly this). A Yellow item is a
///    RUNTIME-incomplete item, not an un-emittable one.
///  - RED: the item itself is inadmissible, or something load-bearing it
///    depends on is Red. It cannot be emitted as written.
///
///===--------------------------------------------------------------------===//
/// The load-bearing asymmetry
///===--------------------------------------------------------------------===//
///
/// Poison does NOT propagate uniformly, and the reason is a property of Rust
/// emission, not a modelling choice:
///
///  - A missing FUNCTION can be replaced. A stub with the right signature and
///    an `unimplemented!()` body type-checks at every call site, so a caller
///    of a Red function still compiles. Call-poisoning therefore only demotes
///    the caller to YELLOW. This is not a guess about what a repair COULD do:
///    it is precisely what `ImportOptions::recover` already does (FR-42).
///  - A missing TYPE cannot be replaced. A record's size, its field names and
///    types, and its `Default`/`Copy` derives are load-bearing at every single
///    use — a local declaration, a `sizeof`, a parameter, a field of another
///    record. There is no stand-in with the right observable behavior, so
///    type-poisoning is transitive and turns every dependent RED.
///
/// Stated as one rule rather than two special cases, which is how it is
/// implemented: an edge whose target is Red poisons its source RED unless the
/// target is STUB-REPLACEABLE, in which case it poisons it only YELLOW. The
/// stub-replaceable items are exactly the functions whose SIGNATURE still
/// maps — again FR-42's own rule, quoted rather than reinvented. Everything
/// else the importer would DROP: a rejected record, enum, or global leaves no
/// substitute behind, so a dependent of one is Red.
///
/// Three consequences fall out of that single rule, and each is a case a
/// hand-written two-rule version would have had to get right separately:
///
///  1. `BodyType`. A Red record used only inside a function's body turns that
///     function RED, not Yellow: the body mentions the type (a local of it, a
///     `sizeof` of it, a cast to it) and there is no way to write those
///     statements without it. Demoting only to Yellow would claim the function
///     emits, which is false. But the function stays STUB-REPLACEABLE — its
///     signature never mentioned the Red type — so ITS callers are only
///     Yellow. Red bodies stop at the function boundary; that boundary is
///     exactly where a stub can be inserted.
///  2. `SigType`. A Red record in a function's SIGNATURE turns the function
///     Red AND unstubbable: the stub would have to spell the missing type in
///     its own parameter list. So the function's callers are RED, not Yellow.
///     The two type edges therefore differ in their effect on the CALLERS,
///     which is the distinction the model exists to make.
///  3. `ReadsGlobal`/`WritesGlobal`. A Red global is dropped, not stubbed, so
///     a function touching one is Red — but again only body-deep, so that
///     function is still stubbable and its own callers are Yellow.
///
///===--------------------------------------------------------------------===//
/// Seeds: the admissibility probe
///===--------------------------------------------------------------------===//
///
/// The Red seeds come from `probeAdmissibility`, a conservative, purely
/// syntactic screen for constructs the importer rejects BY DESIGN — not a
/// reimplementation of the importer's analysis. It deliberately
/// UNDER-approximates: it may call an item admissible that a real import
/// later rejects (a flow-sensitive pointer rejection, say), but it must never
/// call an importable item inadmissible. A false Green is repaired by the
/// search that sits above this (FR-43, which imports candidate subsets for
/// real); a false Red would silently amputate a translatable subset and no
/// later stage could ever recover it. Anything the screen is unsure about is
/// left Green on purpose — see ItemColoring.cpp for the enumerated list of
/// constructs knowingly left Green.
///
///===--------------------------------------------------------------------===//
/// Determinism
///===--------------------------------------------------------------------===//
///
/// A search above this must be reproducible, so every step is a function of
/// the graph's CONTENT alone:
///  - The color fixpoint is a LEAST fixpoint of a monotone rule set, iterated
///    to saturation. Its result does not depend on the order nodes or edges
///    are visited in, and therefore not on the order translation units were
///    given in.
///  - The poison chain is not "whoever got there first" — that WOULD depend on
///    iteration order. Each Red item gets a `rank`, the least number of poison
///    steps from an inadmissible seed (itself a least fixpoint), and its
///    blamed neighbor is the smallest-by-(edge kind, symbol) successor one
///    rank closer to the seed. Ranks strictly decrease along a chain, so the
///    walk terminates even though the graph has cycles.
///  - Every published container is sorted by content; no hash-map iteration
///    order reaches the boundary.
///
///===--------------------------------------------------------------------===//
/// Print format
///===--------------------------------------------------------------------===//
///
/// `ItemColoring::print`, and `emitrust-cc --emit=coloring`: one line per
/// item, in symbol order, then a single tally line. Every field is a whole
/// `key=value` token separated by one space, so FileCheck patterns and
/// `grep ' color=red '` work on whole tokens.
///
/// \code
/// item <symbol> kind=<function|record|enum|global> color=<green|yellow|red> \
///      reason=<reason> [via=<symbol> edge=<EdgeKind> chain=<a>-><b>-><c>] \
///      [construct=<tag>]
/// tally green=<n> yellow=<n> red=<n>
/// \endcode
///
/// (each real item line is unwrapped.) The optional groups appear exactly as
/// follows, which is also the full `reason` vocabulary:
///
///  - `reason=admissible` — Green. No further tokens.
///  - `reason=inadmissible construct=<tag>` — Red, and the item is itself the
///    blocker. `<tag>` names the construct the probe found.
///  - `reason=red-type` — Red because a record/enum it depends on is Red.
///  - `reason=red-global` — Red because a global it accesses is Red.
///  - `reason=red-callee` — Red because a function it calls is Red AND that
///    function is not stub-replaceable.
///  - `reason=stub-callee` — YELLOW: a function it calls is Red but IS
///    stub-replaceable.
///
/// For the four poisoned reasons, `via` is the immediate neighbor to blame,
/// `edge` is the edge kind that carried the poison, `chain` is the whole path
/// from this item to the inadmissible item at the root (`->`-joined, always
/// starting with this item and ending with the root), and `construct` is that
/// ROOT's tag. A Yellow item's chain crosses the stub boundary once, at its
/// first step, and is a Red chain from there on. That chain is the actionable
/// part of the output: it names the one construct whose support would unblock
/// this item.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_PROJECT_ITEMCOLORING_H
#define EMITRUST_PROJECT_ITEMCOLORING_H

#include "EmitRust/Project/ItemGraph.h"

#include "mlir/Support/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <map>
#include <string>
#include <vector>

namespace clang {
class ASTUnit;
} // namespace clang

namespace mlir {
namespace emitrust {

//===----------------------------------------------------------------------===//
// The lattice
//===----------------------------------------------------------------------===//

/// An item's color. The three values form a lattice ordered
/// Green < Yellow < Red under "how blocked is this item"; the fixpoint only
/// ever moves an item UP it, which is what makes the iteration terminate.
enum class ItemColor {
  /// The item and its whole dependency closure are inside the subset.
  Green,
  /// The item emits and compiles, but at least one function it calls is Red
  /// and will be a stub.
  Yellow,
  /// The item cannot be emitted as written.
  Red,
};

/// Why an item has the color it has. Exactly one applies to each item, and
/// the value determines which optional tokens the printed line carries.
enum class ColorReason {
  /// Green: nothing is wrong with the item or anything it depends on.
  Admissible,
  /// Red: the admissibility probe rejected the item itself.
  Inadmissible,
  /// Red: a record or enum the item depends on (`SigType`, `BodyType`,
  /// `Field`, or `Base`) is Red, and a type has no stand-in.
  RedType,
  /// Red: a global the item reads or writes is Red, and a rejected global is
  /// dropped rather than stubbed.
  RedGlobal,
  /// Red: a function the item calls (or takes the address of) is Red AND not
  /// stub-replaceable, so not even a stub can stand in for it.
  RedCallee,
  /// Yellow: a function the item calls (or takes the address of) is Red but
  /// IS stub-replaceable, so the item still compiles against the stub.
  StubCallee,
};

/// The stable spelling of `color` used in `color=<...>` tokens: lowercase.
llvm::StringRef itemColorName(ItemColor color);

/// The stable spelling of `reason` used in `reason=<...>` tokens: lowercase,
/// hyphenated.
llvm::StringRef colorReasonName(ColorReason reason);

/// One item's color, with the blame chain that explains it.
struct ColoredItem {
  /// The item's symbol: a node key of the graph this was computed from, and
  /// therefore the emitted Rust item's name.
  std::string symbol;
  /// The item's kind, copied from its graph node.
  ItemKind kind;
  /// The color.
  ItemColor color;
  /// Which rule produced that color.
  ColorReason reason;
  /// The immediate neighbor to blame, or empty when there is none (a Green
  /// item, or a Red item that is itself the blocker).
  std::string via;
  /// The edge kind that carried the poison from `via`. Meaningful only when
  /// `via` is non-empty.
  EdgeKind viaEdge = EdgeKind::Calls;
  /// The blame path, from this item to the inadmissible item at the root.
  /// Empty for a Green item; `{symbol}` for an item that is itself the
  /// blocker; otherwise at least two entries, the first being `symbol` and
  /// the second `via`.
  std::vector<std::string> chain;
  /// The construct tag of `chain`'s last element — the one construct whose
  /// support would unblock this item. Empty for a Green item.
  std::string construct;
};

/// The whole project's coloring: a deterministic, fully sorted value.
struct ItemColoring {
  /// Every item of the graph this was computed from, ordered by `symbol` —
  /// the item graph's own node order, so a coloring line and a `node` line of
  /// `--emit=item-graph` can be joined position by position. The order is
  /// re-established by `computeColoring` rather than inherited, so that a
  /// caller handing in a graph whose vectors are in any other order still gets
  /// byte-identical output.
  std::vector<ColoredItem> items;

  /// The number of items of each color, for the tally line.
  unsigned countOf(ItemColor color) const;

  /// Renders the coloring in the line format documented at the top of this
  /// file: one `item` line per item, then one `tally` line. Pure; depends on
  /// nothing but `items`.
  std::string print() const;
};

//===----------------------------------------------------------------------===//
// The admissibility probe
//===----------------------------------------------------------------------===//

/// The probe's verdict on one item.
struct AdmissibilityVerdict {
  /// False when the probe found a construct the importer rejects by design.
  bool admissible = true;
  /// False when what the probe found is in the item's SIGNATURE, i.e. when
  /// not even a signature-preserving stub could be written for it. Always
  /// true when `admissible` is; only ever consulted for functions, since no
  /// other item kind is stub-replaceable at all.
  bool signatureAdmissible = true;
  /// The construct tag, from the fixed vocabulary documented in
  /// ItemColoring.cpp (`base-class`, `virtual-method`, `reference-type`, ...).
  /// Empty exactly when `admissible` is true. When several constructs apply
  /// to one item, the lexicographically smallest tag is kept, so the verdict
  /// does not depend on the order declarations were walked in.
  std::string construct;
};

/// The probe's verdicts, keyed by the item graph's node symbols.
///
/// Only INADMISSIBLE items are stored: admissibility is the default, so an
/// absent symbol means Green. That default is also the safe one for the
/// symbols the probe and the graph could ever disagree about — a node with no
/// verdict is admissible, never inadmissible.
class ItemAdmissibility {
public:
  /// Records that `symbol` is inadmissible because of `construct`.
  ///
  /// Repeated calls MERGE rather than overwrite, because one symbol can have
  /// several declarations across the project and the importer sees them all:
  /// `signatureAdmissible` is the AND of every call's, and `construct` keeps
  /// the lexicographically smallest tag. Both are commutative and idempotent,
  /// so the merged verdict is independent of the walk order.
  ///
  /// \param symbol the item graph node key.
  /// \param construct the construct tag; must not be empty.
  /// \param signatureLevel whether the construct is in the item's signature.
  void reject(llvm::StringRef symbol, llvm::StringRef construct,
              bool signatureLevel);

  /// The verdict for `symbol`, or null when the probe found nothing wrong.
  const AdmissibilityVerdict *lookup(llvm::StringRef symbol) const;

  /// Every inadmissible item, ordered by symbol.
  const std::map<std::string, AdmissibilityVerdict> &getVerdicts() const {
    return verdicts;
  }

private:
  /// `std::map` rather than a `StringMap`: the container is ALREADY in the
  /// published order, so no unordered data ever exists to leak.
  std::map<std::string, AdmissibilityVerdict> verdicts;
};

/// Runs the admissibility probe over already-parsed translation units.
///
/// The walk mirrors `buildItemGraph`'s exactly — same implicit-declaration
/// and system-header filtering, same transparent recursion through
/// `extern "C"` and `namespace`, same symbol naming through
/// `EmitRust/CSymbolNaming.h` — so the keys it produces are the graph's node
/// keys by construction.
///
/// This is a pure function of the ASTs: it builds nothing, mutates nothing,
/// and emits no diagnostics. Failure is reserved for a genuine internal error
/// (a null unit).
///
/// \param units the parsed translation units, in project order.
/// \returns the verdicts, or failure if any unit is null.
FailureOr<ItemAdmissibility>
probeAdmissibility(llvm::ArrayRef<clang::ASTUnit *> units);

//===----------------------------------------------------------------------===//
// The fixpoint
//===----------------------------------------------------------------------===//

/// The functional core: colors `graph`'s items given `probe`'s seeds.
///
/// Pure, total, and deterministic — it touches no AST, no filesystem, and no
/// global state, so it is the natural unit to test the fixpoint's
/// order-independence on (shuffle `graph.nodes` and `graph.edges` and the
/// result is unchanged, since only content is consulted).
///
/// \param graph the project's item graph (FR-40).
/// \param probe the per-item admissibility verdicts; symbols absent from it
///        are admissible.
/// \returns one `ColoredItem` per graph node, in the graph's node order.
ItemColoring computeColoring(const ItemGraph &graph,
                             const ItemAdmissibility &probe);

//===----------------------------------------------------------------------===//
// Entry points
//===----------------------------------------------------------------------===//

/// Probes and colors an already-parsed project in one step.
///
/// \param units the parsed translation units, in project order.
/// \returns the coloring, or failure if any unit is null.
FailureOr<ItemColoring> colorItems(llvm::ArrayRef<clang::ASTUnit *> units);

/// Parses `paths` as an independent translation unit each — through the same
/// shell `buildItemGraph` and `importCProject` use, so per-input language
/// selection and `extraClangArgs` behave identically — and colors the result.
///
/// \param paths the source files of the project, in order.
/// \param extraClangArgs additional clang arguments applied to every unit.
/// \returns the coloring, or failure if any input failed to parse (clang has
///          already printed the parse diagnostics to stderr).
FailureOr<ItemColoring> colorItems(llvm::ArrayRef<std::string> paths,
                                   llvm::ArrayRef<std::string> extraClangArgs);

/// `colorItems` driven by a `compile_commands.json` (FR-45), with exactly the
/// semantics `buildItemGraph`'s database overload has: each input's language
/// and flags come from its recorded entry, and an empty `paths` with a
/// database given means "every file the database lists".
///
/// \param compilationDatabasePath a directory holding a
///        `compile_commands.json`, the JSON file itself, or empty for none.
/// \param error receives the reason when the database cannot be loaded.
/// \returns the coloring, or failure (with `error` set only for a database
///          load failure; parse diagnostics go to stderr as usual).
FailureOr<ItemColoring> colorItems(llvm::ArrayRef<std::string> paths,
                                   llvm::ArrayRef<std::string> extraClangArgs,
                                   llvm::StringRef compilationDatabasePath,
                                   std::string &error);

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_PROJECT_ITEMCOLORING_H

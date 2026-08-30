//===- ImportC.h - C-to-EmitRust importer entry point -----------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file declares the public entry point of the C importer: a function
/// that parses a C source file with clang LibTooling and translates the
/// supported C subset into a hybrid MLIR module of core (func/arith/memref/
/// cf) and EmitRust dialect operations.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_IMPORTC_H
#define EMITRUST_IMPORTC_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/OwningOpRef.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <set>
#include <string>

namespace mlir {
class MLIRContext;

namespace emitrust {

//===----------------------------------------------------------------------===//
// Recoverable import (FR-42)
//===----------------------------------------------------------------------===//

/// One top-level declaration that the importer rejected and then recovered
/// from, in the order the declaration walk reached it.
///
/// The record is deliberately self-describing rather than a back-pointer to
/// the clang AST or to the emitted IR: both are gone by the time a driver
/// prints the summary (the ASTs die with `importCProject`, and a dropped
/// item has no IR at all), so everything a report needs is copied out here.
struct RejectedItem {
  /// The MLIR symbol name the item would have claimed — the actual
  /// `func.func` symbol when a stub was emitted, and otherwise the source
  /// spelling of the rejected declaration (`<anonymous>` for a declaration
  /// with no name, which only unnamed records can be).
  std::string symbol;
  /// Where the rejection was raised. This is the location the importer's own
  /// diagnostic carried — a `FileLineColLoc` for every located rejection,
  /// which is all of them in practice — so the ledger points at the C
  /// construct, not at the enclosing declaration.
  Location loc;
  /// The verbatim text of the importer diagnostic, with no `file:line:col:`
  /// prefix and no severity word. Stored verbatim (rather than re-worded)
  /// so a recovered build and a hard-failing build report the same string,
  /// and so `classifyBlocker` sees exactly what the RealWorld harness sees.
  std::string diagnostic;
  /// The coarse blocker category from `classifyBlocker`.
  std::string blockerTag;
  /// True when the item was a function whose signature still mapped and an
  /// `unimplemented!()` stub carrying that signature was emitted in its
  /// place; false when the item was dropped from the module entirely.
  bool stubbed = false;
  /// FR-49: the FR-40 item-graph node key of the record that ENCLOSES this
  /// item, when the rejected declaration is an out-of-line C++ member
  /// function of a file-scope class; empty for everything else (which is
  /// every C item).
  ///
  /// A member function is not itself a graph node (`ItemGraph.h` documents
  /// why), so a rejection of one cannot be joined to the FR-41 coloring on
  /// its own symbol — and `symbol` here is the bare member spelling
  /// (`area_x100`, `~Rect`), which three sibling classes may share. Its
  /// enclosing CLASS is a node, is colored, and carries the poison chain
  /// that names the real root blocker, so this field is the join key that
  /// makes root-cause attribution possible for off-graph items. It is
  /// recorded by the importer rather than recovered by name later precisely
  /// because the name cannot be recovered: nothing in `area_x100` says
  /// `Rect`.
  std::string ownerSymbol;
  /// FR-126: when this rejection is a rejected-type-cascade
  /// restatement ("struct 'X' was rejected, so a type naming it..."), the
  /// graph key of the rejected TYPE itself, recorded by the importer at the
  /// cascade emit site; empty otherwise. Report-side attribution follows
  /// this key to the type's own ledger row / coloring to find the real root.
  std::string cascadeSourceSymbol;
};

/// Maps one verbatim importer diagnostic to a coarse blocker category.
///
/// The tag vocabulary is deliberately IDENTICAL to `classify_blocker` in
/// `test/RealWorld/run_realworld.py`, which tags whole-program rejections
/// for the RealWorld corpus survey: the two ledgers must speak the same
/// language so a per-item recovery report and a per-program survey can be
/// tabulated together. The heuristic is therefore a direct port —
/// system-header symbol names split into `dynamic-memory` (the allocator
/// family) and `libc:<name>`, then an ordered substring table, then the two
/// wordings that several blockers share, disambiguated by reading the cited
/// source line.
///
/// `loc` supplies that cited line: the Python side re-parses `file:line:col`
/// out of the diagnostic text, while in-process the location is already a
/// `FileLineColLoc`, so the ambiguous-wording refinement reads the file
/// directly. A non-file location simply skips the refinement and yields the
/// same default the Python heuristic reaches on an unreadable file.
///
/// The Python side additionally has a `crash` tag for a clang/importer crash
/// observed through a subprocess exit; an in-process importer cannot observe
/// its own crash, so that tag is never produced here (the vocabulary still
/// contains it, in the survey).
///
/// Symmetrically, two tags are produced ONLY here, from wordings a
/// non-recovering whole-program run can never emit: `search-excluded`
/// (FR-43's synthetic exclusion) and `cxx-cascaded-method` (FR-49; a C++
/// member function whose class was already rejected and dropped). Both are
/// tested before the shared tables and are absent from the survey's, which
/// keeps the two vocabularies in step rather than adding dead entries there.
///
/// \param diagnostic the verbatim diagnostic message (no location prefix).
/// \param loc the diagnostic's location, used for the ambiguous wordings.
/// \returns the blocker tag, never empty (`other` is the fallback).
std::string classifyBlocker(llvm::StringRef diagnostic, Location loc);

/// The rejections a recovering import accumulated.
///
/// This is a plain append-only value: the importer owns no ledger of its own,
/// the caller hands one in through `ImportOptions`, and it stays valid and
/// readable after the import returns. Keeping it out of the importer is what
/// lets a driver print a summary after the module has already been lowered.
class RejectionLedger {
public:
  /// Appends one recovered rejection.
  void record(RejectedItem item) { items.push_back(std::move(item)); }

  /// The recorded rejections, in declaration-walk order.
  llvm::ArrayRef<RejectedItem> getItems() const { return items; }

  /// True when the import rejected nothing (the recovering import produced
  /// exactly what a non-recovering one would have).
  bool empty() const { return items.empty(); }

  /// Rejections per blocker tag, ordered by tag. `std::map` rather than a
  /// `StringMap` so the tabulation order is deterministic without a sort at
  /// every print, matching the survey's sorted output.
  std::map<std::string, unsigned> tally() const;

  /// Prints a human-readable summary: one line per rejected item (symbol,
  /// location, tag, whether a stub replaced it, and the diagnostic), then a
  /// per-tag tabulation. Prints nothing at all when the ledger is empty, so
  /// a driver can call it unconditionally.
  void printSummary(llvm::raw_ostream &os) const;

private:
  llvm::SmallVector<RejectedItem> items;
};

//===----------------------------------------------------------------------===//
// External requirements (FR-52)
//===----------------------------------------------------------------------===//

/// What `finalizeProject` does with a non-variadic external FUNCTION that
/// some translation unit references and none defines.
///
/// The fact itself is a property of the whole project, not of any one
/// declaration, which is why FR-42's per-item recovery cannot absorb it: no
/// single item can be dropped to make the symbol appear. What CAN change is
/// whether the fact is an ERROR or a REQUIREMENT — and which of the two it is
/// depends entirely on what the crate is for.
enum class ExternalRequirements {
  /// Historical behavior: a located error at the symbol's first use site.
  Reject,
  /// FR-52: record the declaration as a requirement — but only if the module
  /// defines no `c_main`, i.e. only if the crate emitted from it will be a
  /// LIBRARY (the same predicate `selectCrateType` uses, so the importer and
  /// the crate emitter cannot disagree). A module WITH an entry point keeps
  /// the rejection: see `Trait` for why.
  TraitWhenLibrary,
  /// FR-52: always record the declaration as a requirement, even for a module
  /// that defines `c_main`. Only correct when the caller has already decided
  /// to emit a library crate (`--crate-type=lib`), because a BINARY crate's
  /// `fn main` is not generic and has no caller to supply the impl.
  Trait,
};

/// Knobs shared by `importC` and `importCProject`.
///
/// Defaulting every field to the historical behavior is the point: the
/// three-argument entry points below construct a default `ImportOptions` and
/// are therefore byte-for-byte the import they always were.
struct ImportOptions {
  /// Recoverable import (FR-42). When false — the default — the first
  /// unsupported top-level declaration fails the whole import, exactly as
  /// before. When true, such a declaration is recorded in `ledger`, reported
  /// as a WARNING instead of an error, and the declaration walk continues:
  /// a function whose signature still maps is replaced by a stub with that
  /// signature and an `unimplemented!()` body (so its callers still compile),
  /// and anything else is dropped. Every partial IR the rejected item built
  /// is discarded before the walk resumes.
  ///
  /// Recovery covers the top-level declaration walk AND the two Pass-A
  /// planners that can reject (`planCursorParams`, `planVaMonomorph`, via
  /// FR-53); the other seven planners return `void` and cannot reject at all.
  /// A rejection raised anywhere else -- project finalization, module
  /// verification, a clang parse error -- still fails the import.
  ///
  /// An earlier version of this comment justified the planner exclusion by
  /// claiming such rejections "cannot be attributed to a single droppable
  /// item". That was wrong: all seven planner `emitError` sites carry a
  /// location inside one declaration, and FR-53 attributes every one of them.
  /// The claim survived because nothing in the self-authored corpus exercised
  /// it; on third-party C it was the single largest cause of lost output,
  /// costing 14 of 22 parsed translation units.
  ///
  /// Recovery still stops at the IMPORT boundary. A failure in the conversion
  /// pipeline or the Rust emitter (an un-legalizable `scf.if`, a non-finite
  /// float constant) still costs the whole crate, which is the same
  /// structural shape one layer down and is now the leading blocker.
  bool recover = false;
  /// Where recovered rejections are recorded. May be null even with
  /// `recover` set, in which case the rejections are still warned about but
  /// not collected.
  RejectionLedger *ledger = nullptr;
  /// A `compile_commands.json` supplying each input's real command line and
  /// language (FR-45): either a directory containing the database or the
  /// JSON file itself. Empty — the default — keeps the historical
  /// per-extension language guess.
  ///
  /// This lives in the options rather than as a separate parameter because
  /// it composes with `recover` along the axis that matters: a real C++
  /// project needs BOTH a database (to be parsed the way its build system
  /// parses it) and recovery (to yield anything at all). Overloads crossing
  /// the two would multiply, and the driver would still want them together.
  ///
  /// With `importCProject` and an EMPTY `paths`, a database given here means
  /// the project IS the database: every file it lists is imported, sorted
  /// and deduplicated.
  std::string compilationDatabasePath;
  /// The items the caller has decided NOT to admit (FR-43). Each entry is an
  /// FR-40 item-graph node key, i.e. the symbol the item would be emitted
  /// under; a top-level declaration whose key is listed here is not imported
  /// at all and takes the ORDINARY recovery path instead — a function whose
  /// signature maps becomes an `unimplemented!()` stub, anything else is
  /// dropped — with the reason `excluded by the search state`.
  ///
  /// This is the only knob FR-43's frontier search needs from the importer:
  /// a search STATE is a set of admitted items, and probing that state means
  /// importing with its complement excluded. Exclusion is expressed as a
  /// synthetic REJECTION rather than as a fourth outcome so that a search
  /// probe and an ordinary recovering import produce the same shapes of
  /// module, ledger, and report — the search never has to model a kind of
  /// item the rest of the system has not already seen.
  ///
  /// It has effect ONLY together with `recover`; the recovery path is where
  /// the substitution happens, and a non-recovering import ignores the set
  /// entirely (a hard-failing import has no partial answer to give). An
  /// empty set — the default — is exactly the historical import.
  ///
  /// The keys are `tu<i>_`-tagged for internal-linkage items, matching
  /// `importCProject` and `buildItemGraph`. `importC`'s single-file mode
  /// emits file-statics under their bare names, so an excluded file-static
  /// is only addressable through `importCProject`; every FR-43 caller goes
  /// through the project entry point, where the two namings agree by
  /// construction.
  ///
  /// `std::set` rather than a `StringSet`: nothing here is hot (one lookup
  /// per top-level declaration), and an ordered container keeps every
  /// derived listing deterministic without a sort at the boundary.
  std::set<std::string> excludedItems;
  /// FR-52: what to do with a referenced-but-undefined external FUNCTION.
  /// `Reject` — the default — is the historical whole-program error, so every
  /// existing caller is unaffected.
  ///
  /// Under either trait setting the declaration is KEPT in the module as a
  /// body-less `func.func` carrying `emitrust.external_requirement`, and the
  /// `emitrust-lower-external-requirements` pass later turns the marked set
  /// into one `emitrust.trait_def` plus a type parameter on the transitive
  /// closure of their callers. A module that reaches the Rust emitter with
  /// the marker still on it fails exactly as an unresolved external always
  /// did, so forgetting the pass cannot silently emit a broken crate.
  ///
  /// Three shapes are NOT recorded even under a trait setting, each because
  /// the trait cannot faithfully express them:
  ///  - a declaration whose ADDRESS is taken anywhere. A function pointer
  ///    renders as `Some(<name>)`, an opaque constant that names the item
  ///    directly rather than through the trait's type parameter;
  ///  - a C++ MEMBER function (`emitrust.method_of`). Its call sites are
  ///    receiver-bearing `emitrust.method_call`s, and an associated trait
  ///    function has no receiver to bind them to. A missing method body is
  ///    also a hole in code the project OWNS, not a requirement on its
  ///    environment;
  ///  - an undefined external GLOBAL, which rejects unconditionally: an
  ///    associated const is a value, not storage, so it cannot carry the
  ///    mutable object a C `extern int` denotes.
  /// Undefined VARIADIC functions never reach this decision at all — a
  /// body-less variadic declaration is not imported, and its call sites and
  /// address-takes carry their own located rejections.
  ExternalRequirements externalRequirements = ExternalRequirements::Reject;
  /// FR-57a: deferred-externals import mode, for the per-TU FR-56/FR-57 shim
  /// path. When false — the default — a referenced external symbol that no
  /// imported translation unit defines is the historical whole-program
  /// error (or, for functions, the FR-52 trait requirement when that policy
  /// is chosen). When true, such a symbol becomes a DECLARATION instead: an
  /// undefined extern global is materialized as a declaration-only
  /// `emitrust.global` (no initializer) and a referenced body-less function
  /// keeps its declaration, both carrying the `emitrust.extern_decl` unit
  /// attribute. Defer takes precedence over the FR-52 trait policy.
  ///
  /// The resulting module is a per-TU SHARD, not a program: the FR-58
  /// link/merge step must resolve every marked declaration against its
  /// defining translation unit, and the Rust emitter refuses a module still
  /// carrying the marker with a located error, so direct crate emission of
  /// a deferred module cannot silently produce a crate that reads a symbol
  /// nobody defines.
  bool deferExternals = false;
};

/// Imports the C source file at `path` into an MLIR module.
///
/// The file is parsed as C11 with clang. Scalar locals and control flow are
/// translated to core dialects (rank-0 `memref.alloca` cells, `arith`
/// operations, and `cf` branches) so that upstream passes (`--mem2reg`,
/// `--lift-cf-to-scf`) can recover structured, SSA-form IR. Aggregates and
/// pointers are translated directly to EmitRust place operations
/// (`emitrust.variable`/`member`/`subscript`/`deref`/`load`/`assign`/
/// `addr_of`), and complete named struct definitions become module-level
/// `emitrust.struct_def` operations. The C `main` function is renamed to
/// `c_main`.
///
/// The required dialects (emitrust, func, arith, memref, cf) are loaded into
/// `context` by this function. Every C construct outside the supported
/// subset produces a diagnostic carrying a `FileLineColLoc` source location;
/// no silently wrong IR is ever produced. The returned module has been
/// verified.
///
/// `extraClangArgs` are appended to the clang command line after `-std=c11`,
/// in the order given, so a caller can pass include-path flags (`-I<dir>`,
/// `-isystem <dir>`) and any other clang driver arguments. When empty, the
/// command line is exactly `-std=c11` plus the configured resource dir, so
/// this preserves the historical single-file behavior.
///
/// \param path the path of the C source file to import.
/// \param extraClangArgs additional clang command-line arguments.
/// \param context the MLIR context that owns the created module.
/// \returns the imported module, or null on failure with diagnostics already
///          emitted through `context`'s diagnostic engine (parse errors are
///          printed to stderr by clang).
OwningOpRef<ModuleOp> importC(llvm::StringRef path,
                              llvm::ArrayRef<std::string> extraClangArgs,
                              MLIRContext &context);

//===----------------------------------------------------------------------===//
// Printing an imported module (FR-135)
//===----------------------------------------------------------------------===//

/// Prints `module` as MLIR text that can be READ BACK, and reports it when
/// that costs a change of form.
///
/// `--emit=import` (and `emitrust-import-c`) exist so a person or a bisecting
/// script can re-read the front end's module. That was not always possible:
/// MLIR's `cf.switch` CUSTOM assembly prints each case label with
/// `APInt::getLimitedValue()`, i.e. UNSIGNED, while its own parser reads
/// labels with `parseInteger(int64_t)` — so a label whose zero-extension
/// needs the 64th bit prints as a decimal the same parser then rejects with
/// `custom op 'cf.switch' integer value too large`. The defect is UPSTREAM
/// and the IR is correct: the GENERIC form of the same op prints
/// `case_values = dense<-2>` and round-trips exactly.
///
/// So when the module contains such a label this prints the whole module in
/// the generic form — MLIR's printing flags are module-wide, there is no
/// per-op escape — and emits a LOCATED remark saying so. A module with no
/// such label is printed exactly as it always was, byte for byte; the
/// fallback must never reach the common path, which is why
/// `test/Driver/emit-import-roundtrip.c` pins both halves.
///
/// \param module the module to print.
/// \param os the stream to print it to.
void printRoundTrippableModule(ModuleOp module, llvm::raw_ostream &os);

/// Overload importing a single file whose clang command line comes from a
/// `compile_commands.json` (FR-45).
///
/// `compilationDatabasePath` names either the database file itself or a
/// directory containing one; when it is empty this is exactly the overload
/// above (extension-guessed language, `extraClangArgs` only). When it is
/// set, `path`'s recorded entry supplies the command line — its `-I`,
/// `-D`, `-std`, and `-x` flags, resolved against the entry's `directory`
/// — with driver-only arguments (`-c`, `-o`, `-M*`) filtered out, and
/// `extraClangArgs` appended last so a caller can still override. A file
/// the database does not mention falls back to the extension guess.
///
/// \param path the path of the C or C++ source file to import.
/// \param extraClangArgs additional clang command-line arguments, appended
///        after the database's own.
/// \param compilationDatabasePath a `compile_commands.json` or its
///        directory; empty selects the extension-guessing behavior.
/// \param context the MLIR context that owns the created module.
/// \returns the imported module, or null on failure (including a database
///          that cannot be loaded) with diagnostics already emitted.
OwningOpRef<ModuleOp> importC(llvm::StringRef path,
                              llvm::ArrayRef<std::string> extraClangArgs,
                              llvm::StringRef compilationDatabasePath,
                              MLIRContext &context);

/// Convenience overload importing a single file with no extra clang args.
OwningOpRef<ModuleOp> importC(llvm::StringRef path, MLIRContext &context);

/// `importC` under explicit `options`. The three-argument overload above is
/// exactly this one with a default-constructed `ImportOptions`, so recovery
/// is opt-in per call and every existing caller keeps its behavior.
OwningOpRef<ModuleOp> importC(llvm::StringRef path,
                              llvm::ArrayRef<std::string> extraClangArgs,
                              const ImportOptions &options,
                              MLIRContext &context);

/// Imports a whole C project: parses every source file in `paths` as an
/// independent C11 translation unit and merges them into one MLIR module,
/// resolving external symbols across translation units and keeping
/// internal-linkage (`static`) symbols distinct.
///
/// Linkage model:
///  - External-linkage functions and globals keep their bare C name and are
///    unified program-wide; a prototype in one TU is satisfied by a
///    definition in another. Two definitions of the same external symbol, or
///    a referenced-but-undefined non-variadic external function/global, are
///    located diagnostics.
///  - Internal-linkage (`static` at file scope) functions and globals are
///    mangled with a per-TU prefix so identically named file-statics in
///    different TUs never collide.
///  - Aggregate definitions (`struct`/`enum`) shared through a header are
///    deduplicated by symbol name; a name reused with a different shape is a
///    located diagnostic.
///
/// \param paths the C source files to merge (at least one, all non-null ASTs).
/// \param extraClangArgs additional clang command-line arguments (include
///        paths etc.), applied to every translation unit.
/// \param context the MLIR context that owns the created module.
/// \returns the merged, verified module, or null on failure with diagnostics
///          already emitted.
OwningOpRef<ModuleOp> importCProject(llvm::ArrayRef<std::string> paths,
                                     llvm::ArrayRef<std::string> extraClangArgs,
                                     MLIRContext &context);

/// Convenience overload naming only a `compile_commands.json` (FR-45),
/// equivalent to setting `ImportOptions::compilationDatabasePath`. Kept as
/// its own overload so a caller that wants nothing but a database does not
/// have to construct an options object.
///
/// \param paths the source files to merge, or empty to take the whole
///        database.
/// \param extraClangArgs additional clang command-line arguments, appended
///        after the database's own.
/// \param compilationDatabasePath a `compile_commands.json` or its
///        directory; empty selects the historical behavior.
/// \param context the MLIR context that owns the created module.
/// \returns the merged, verified module, or null on failure with
///          diagnostics already emitted.
OwningOpRef<ModuleOp> importCProject(llvm::ArrayRef<std::string> paths,
                                     llvm::ArrayRef<std::string> extraClangArgs,
                                     llvm::StringRef compilationDatabasePath,
                                     MLIRContext &context);

/// `importCProject` under explicit `options` — the full entry point.
///
/// The three-argument overload above is exactly this one with a
/// default-constructed `ImportOptions`, so every existing caller keeps its
/// behavior.
///
/// With `options.recover` set, each translation unit recovers independently
/// but into ONE shared ledger, and cross-TU finalization still runs over the
/// surviving symbols: a stub emitted for a rejected definition satisfies the
/// other TUs' prototypes exactly like a real definition would, and a dropped
/// item leaves its prototypes referenced-but-undefined, which
/// `finalizeProject` still rejects (that rejection is a whole-project fact,
/// not a droppable item, so it fails the import).
///
/// With `options.compilationDatabasePath` set, two things change and nothing
/// else:
///
///  - the SOURCE LIST: when `paths` is empty, every file the database lists
///    is imported (sorted by path and deduplicated, so the translation-unit
///    order the module records is deterministic); when `paths` is non-empty
///    only those files are imported, but still with the database's flags.
///  - the COMMAND LINE of each file: its recorded entry supplies the flags
///    and therefore the LANGUAGE (the entry's `-x`/`-std`, or the driver's
///    own inference), resolved against that entry's `directory` field, with
///    driver-only arguments (`-c`, `-o`, `-M*`) filtered out.
///    `extraClangArgs` is appended after the database's arguments so a
///    caller can override. A file the database does not mention falls back
///    to the extension guess.
///
/// An empty `paths` with no database keeps the historical "no C input files
/// given" error.
///
/// \param paths the source files to merge, or empty to take the whole
///        database.
/// \param extraClangArgs additional clang command-line arguments, appended
///        after the database's own, applied to every translation unit.
/// \param options recovery and compilation-database knobs.
/// \param context the MLIR context that owns the created module.
/// \returns the merged, verified module, or null on failure (including a
///          database that cannot be loaded) with diagnostics already
///          emitted.
OwningOpRef<ModuleOp> importCProject(llvm::ArrayRef<std::string> paths,
                                     llvm::ArrayRef<std::string> extraClangArgs,
                                     const ImportOptions &options,
                                     MLIRContext &context);

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_IMPORTC_H

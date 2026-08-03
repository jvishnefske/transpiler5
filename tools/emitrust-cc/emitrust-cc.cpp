//===- emitrust-cc.cpp - C-to-Rust transpiler driver ----------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file is the imperative shell of emitrust-cc, the end-to-end C-to-Rust
/// transpiler driver. It parses the command line, imports the C input with
/// the ImportC library, runs the pinned pass pipeline
/// (mem2reg, canonicalize, lift-cf-to-scf, canonicalize,
/// convert-to-emitrust; --check-range-refinement additionally runs the
/// observational emitrust-range-refinement-check pass on the
/// pre-conversion stage), and emits one of:
///   --emit=item-graph
///                  the FR-40 whole-project program item graph, computed
///                  from the clang ASTs alone — no import, no pass, no
///                  emission — so it is available even for projects the
///                  importer rejects;
///   --emit=coloring
///                  the FR-41 three-color lattice over that graph (which
///                  items are inside the supported subset, which are only
///                  blocked by a stubbable callee, and which are blocked
///                  outright, each with the chain of items to blame), also
///                  pure-AST and for the same reason;
///   --emit=import  the raw imported MLIR module, before any pass;
///   --emit=mlir    the MLIR module after the full pass pipeline;
///   --emit=rust    Rust source text, identical to the crate root
///                  --emit=crate would write (src/main.rs when the input
///                  defines main, else src/lib.rs);
///   --emit=crate   a complete cargo crate directory (the default) — a
///                  BINARY crate when the input defines main and a LIBRARY
///                  crate when it does not (FR-51; --crate-type overrides
///                  the choice) — with --build optionally invoking
///                  `cargo build --release --offline` on the result, and
///                  --incremental (FR-44) additionally recovering from
///                  unsupported items and writing PORTING.md /
///                  emitrust-progress.json beside the crate;
///   --emit=search  the FR-43 frontier-search trace alone — which candidate
///                  subsets were tried, what each import attempt learned,
///                  and which subset won — for a project that need not
///                  define main.
/// With --search, --emit=crate --incremental picks the crate's item set with
/// that same search instead of taking whatever the first recovering import
/// happened to accept.
/// With --link (FR-58 slice 1) there is no import and no pass pipeline at
/// all: the positional inputs are OBJECT files carrying per-TU emitrust
/// shards (`.emitrust` section, `.emitrust.mlirbc` sidecar fallback) or
/// shard files named directly, which are merged in link-line order by the
/// functional core in LinkMerge.h and fed to the same --emit=rust/crate
/// emission below.
/// The inputs are either positional source paths with hand-passed
/// -I/-isystem/--extra-arg flags (the historical surface), or, with
/// --compdb <dir-or-file>, a real compile_commands.json that supplies both
/// the source list and each file's flags and language (FR-45); the two
/// combine, a positional list with --compdb selecting a subset of the
/// database's files.
/// All content rendering is delegated to the pure functions in
/// CrateEmitter.h; this file owns diagnostics, filesystem writes, and
/// process invocation. Any failure produces a located diagnostic and a
/// nonzero exit code; no partial output is ever kept.
//
//===----------------------------------------------------------------------===//

#include "CrateEmitter.h"
#include "LinkMerge.h"
#include "Partition.h"
#include "ProgressReport.h"
#include "RatchetReport.h"

#include "EmitRust/CSymbolNaming.h"
#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/Conversion/ConvertToEmitRust.h"
#include "EmitRust/Conversion/LowerContainers.h"
#include "EmitRust/Conversion/LowerExternalRequirements.h"
#include "EmitRust/Conversion/RangeRefinementCheck.h"
#include "EmitRust/ImportC.h"
#include "EmitRust/Project/FrontierSearch.h"
#include "EmitRust/Project/ItemColoring.h"
#include "EmitRust/Project/ItemGraph.h"
#include "EmitRust/ShardMetadata.h"

#include "mlir/Conversion/ControlFlowToSCF/ControlFlowToSCF.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/Verifier.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/FileUtilities.h"
#include "mlir/Transforms/Passes.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

/// The output kinds selectable with --emit.
///
/// `ItemGraph` and `Coloring` are the odd ones out: every other kind is a
/// stage of the import-and-lower pipeline, while those two are parallel,
/// pure-AST analyses that never build a module. They are handled before the
/// import for exactly that reason — both are meant to be obtainable for a
/// project the importer cannot yet translate, which for the coloring is the
/// whole point (an all-Green project needs no coloring).
enum class EmitKind {
  ItemGraph,
  Coloring,
  Import,
  MLIR,
  Rust,
  Crate,
  Search,
  RejectionReport,
  Ratchet,
};

/// Whether `kind` is one of the pure-AST project analyses, i.e. produced
/// without an MLIR context, an import, or a pass.
///
/// `Search` is deliberately NOT one of them even though it prints an
/// analysis: FR-43's whole method is to IMPORT candidate subsets, so it needs
/// the same machinery an ordinary compile does and simply keeps no module at
/// the end.
bool isProjectAnalysis(EmitKind kind) {
  return kind == EmitKind::ItemGraph || kind == EmitKind::Coloring;
}

} // namespace

// Not `cl::OneOrMore`: with --compdb the database may supply the whole
// source list, so "no positional inputs" is legal there. The historical
// requirement is enforced explicitly in main() instead, which keeps the
// no---compdb command line exactly as strict as it was.
static llvm::cl::list<std::string>
    inputFilenames(llvm::cl::Positional, llvm::cl::desc("<input C files>"));

static llvm::cl::opt<std::string> compilationDatabasePath(
    "compdb",
    llvm::cl::desc(
        "Take the clang command line for each input from a "
        "compile_commands.json: <path> is either the database file itself "
        "or a directory containing one. Each file's flags -- include "
        "paths, macros, and its language (the entry's -x/-std) -- come "
        "from its database entry, resolved against that entry's "
        "'directory'; driver-only arguments (-c, -o, -M*) are filtered "
        "out. With no positional inputs, every file in the database is "
        "imported; with positional inputs, only those are, still with the "
        "database's flags. --extra-arg is appended after the database's "
        "arguments"),
    llvm::cl::value_desc("dir-or-file"), llvm::cl::init(""));

static llvm::cl::list<std::string>
    includeDirs("I", llvm::cl::Prefix,
                llvm::cl::desc("Add a directory to the include search path"),
                llvm::cl::value_desc("dir"));

static llvm::cl::list<std::string> systemIncludeDirs(
    "isystem",
    llvm::cl::desc("Add a directory to the system include search path"),
    llvm::cl::value_desc("dir"));

static llvm::cl::list<std::string>
    extraArgs("extra-arg",
              llvm::cl::desc("Additional clang argument, passed verbatim "
                             "(repeatable)"),
              llvm::cl::value_desc("arg"));

static llvm::cl::opt<std::string> crateNameOpt(
    "crate-name",
    llvm::cl::desc("Name of the emitted cargo crate (defaults to the single "
                   "input's stem, or the -o directory stem for several "
                   "inputs)"),
    llvm::cl::value_desc("name"), llvm::cl::init(""));

static llvm::cl::opt<EmitKind> emitKind(
    "emit", llvm::cl::desc("Output kind"),
    llvm::cl::values(
        clEnumValN(EmitKind::ItemGraph, "item-graph",
                   "Whole-project program item graph (FR-40): one 'node' "
                   "line per function/record/enum/global and one 'edge' "
                   "line per dependency, both sorted; computed from the "
                   "clang ASTs without importing"),
        clEnumValN(EmitKind::Coloring, "coloring",
                   "Three-color lattice over the item graph (FR-41): one "
                   "'item' line per function/record/enum/global giving its "
                   "color (green = it and its whole type closure are inside "
                   "the supported subset, yellow = it compiles but calls a "
                   "stubbed function, red = it cannot be emitted), the reason, "
                   "and for a non-green item the chain of items to blame down "
                   "to the one construct at fault; then a tally line. Also "
                   "computed from the clang ASTs without importing"),
        clEnumValN(EmitKind::Import, "import",
                   "Raw imported MLIR module, before any pass (debugging)"),
        clEnumValN(EmitKind::MLIR, "mlir",
                   "MLIR module after the full pass pipeline, i.e. the "
                   "emitter's input (debugging)"),
        clEnumValN(EmitKind::Rust, "rust",
                   "Rust source text, byte-identical to the crate root "
                   "--emit=crate would write: src/main.rs when the input "
                   "defines main (allow-header, translation, fn main "
                   "wrapper), else src/lib.rs (allow-header and the "
                   "translation with its exported items marked pub). "
                   "--crate-type applies here too"),
        clEnumValN(EmitKind::Crate, "crate",
                   "Complete cargo crate directory; -o names the crate "
                   "directory. An input that defines main emits a binary "
                   "crate, one that does not emits a library crate; see "
                   "--crate-type"),
        clEnumValN(EmitKind::Search, "search",
                   "FR-43 frontier-search trace: the candidate item subsets "
                   "the search tried, what each import attempt learned, the "
                   "subset that won and why every other item is out. Implies "
                   "--search; no crate and no module is written, and the "
                   "project need not define main"),
        clEnumValN(EmitKind::RejectionReport, "rejection-report",
                   "FR-60: aggregate the shard artifacts' FR-57d rejection "
                   "ledgers into the per-construct report that ranks what "
                   "semantic work buys the most frontier -- grouped by the "
                   "classifyBlocker tag, wordings tabulated with quoted "
                   "names normalized, deterministic. Requires --link; no "
                   "merge and no re-import runs, so it stays a pure "
                   "artifact query at kernel scale"),
        clEnumValN(EmitKind::Ratchet, "ratchet",
                   "FR-60: the per-project ratchet manifest -- admitted "
                   "items per shard and total, the FR-59 crate facts "
                   "(with --partition), and the per-tag rejection "
                   "snapshot. With --ratchet-baseline the manifest is "
                   "compared first: an admitted shrink or a condensation "
                   "growth is an error; growth passes, and the written "
                   "manifest is the update. Requires --link")),
    llvm::cl::init(EmitKind::Crate));

static llvm::cl::opt<emitrustcc::CrateTypeRequest> crateTypeOpt(
    "crate-type", llvm::cl::desc("Shape of the emitted crate (FR-51)"),
    llvm::cl::values(
        clEnumValN(emitrustcc::CrateTypeRequest::Auto, "auto",
                   "Decide from the input (the default): a project that "
                   "defines 'main' emits a BINARY crate -- src/main.rs and a "
                   "fn main wrapper, byte-identical to every crate emitted "
                   "before this flag existed -- and one that does not emits a "
                   "LIBRARY crate: src/lib.rs, a [lib] manifest section, no "
                   "wrapper, and 'pub' on the external-linkage items a caller "
                   "outside the crate must be able to reach"),
        clEnumValN(emitrustcc::CrateTypeRequest::Bin, "bin",
                   "Force a binary crate. An input that defines no 'main' is "
                   "a located error rather than a crate that cannot link"),
        clEnumValN(emitrustcc::CrateTypeRequest::Lib, "lib",
                   "Force a library crate even for an input that defines "
                   "'main', which then becomes an ordinary exported function "
                   "('c_main') rather than the crate's entry point")),
    llvm::cl::init(emitrustcc::CrateTypeRequest::Auto));

static llvm::cl::opt<std::string>
    outputPath("o",
               llvm::cl::desc("Output file for --emit=import/mlir/rust "
                              "('-' means stdout); output crate directory "
                              "for --emit=crate (required)"),
               llvm::cl::value_desc("path"), llvm::cl::init("-"));

static llvm::cl::opt<bool> linkFlag(
    "link",
    llvm::cl::desc(
        "FR-58 link step: the positional inputs are OBJECT files produced "
        "by the emitrust-clang shim (their `.emitrust` section carries the "
        "per-TU emitrust shard as MLIR bytecode; an object without the "
        "section falls back to the `<object>.emitrust.mlirbc` sidecar next "
        "to it) or `.mlirbc` shard files named directly, in any mix. The "
        "shards are merged in link-line order per the spike-proven FR-58 "
        "algorithm -- drop each `emitrust.extern_decl` declaration some "
        "shard defines (an obligation nobody defines is the "
        "undefined-symbol link error), dedup shared struct/enum/global "
        "definitions by symbol with first occurrence winning (a "
        "same-symbol shape mismatch is the shape-conflict link error), "
        "alpha-rename per-TU internal-linkage `tu0_` tags to link-line "
        "ordinals, concatenate -- and the merged module feeds the ordinary "
        "crate-emission path. No C source is parsed and no pass pipeline "
        "runs: the shards are already fully converted, which is the whole "
        "point (only --emit=rust and --emit=crate apply)"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> partitionFlag(
    "partition",
    llvm::cl::desc(
        "FR-59: partition the --link result into a Cargo WORKSPACE of "
        "crates instead of one crate -- one library member per source "
        "directory by default (each shard's recorded `emitrust.source` "
        "path decides), the binary member holding the TU that defines "
        "`main`, path dependencies and `use <dep>::*;` imports wiring the "
        "members together through the FR-51 export rules. Boundaries the "
        "packaging cannot express CONDENSE with a warning instead of "
        "failing: dependency cycles (an SCC lands whole in one crate), "
        "globals referenced across the boundary (never exported), impl "
        "blocks away from their type's crate, and crates referencing into "
        "the binary member. Requires --link --emit=crate; without this "
        "flag the single-crate output is byte-identical to before FR-59"),
    llvm::cl::init(false));

static llvm::cl::opt<std::string> partitionMapPath(
    "partition-map",
    llvm::cl::desc(
        "FR-59: override file for --partition. One `<path-prefix> "
        "<crate-name>` pair per line; the LONGEST prefix matching a "
        "shard's recorded source path assigns it to that crate, and "
        "unmatched shards keep the per-directory default"),
    llvm::cl::value_desc("file"), llvm::cl::init(""));

static llvm::cl::opt<std::string> ratchetBaselinePath(
    "ratchet-baseline",
    llvm::cl::desc(
        "FR-60: a committed ratchet manifest to compare --emit=ratchet's "
        "result against BEFORE writing it. An admitted-item shrink (total, "
        "per shard, or a shard vanishing) or a condensation-warning growth "
        "is an error; admitted growth passes with a note, and the written "
        "manifest is the update"),
    llvm::cl::value_desc("file"), llvm::cl::init(""));

static llvm::cl::opt<bool> buildFlag(
    "build",
    llvm::cl::desc("After writing the crate, run 'cargo build --release "
                   "--offline' on it (only valid with --emit=crate)"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> recoverFlag(
    "recover",
    llvm::cl::desc(
        "Recoverable import (FR-42): instead of failing the whole compile at "
        "the first unsupported top-level declaration, record it, report it as "
        "a warning, and keep importing the rest. A rejected function whose "
        "signature still maps is replaced by a stub with that signature and "
        "an unimplemented!() body so its callers still compile; anything else "
        "is dropped. A summary of the recovered rejections is printed to "
        "stderr. Off by default, in which case the compile is byte-identical "
        "to one built without this flag"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> preserveCNamesFlag(
    "preserve-c-names",
    llvm::cl::desc(
        "Emit C symbol spellings verbatim instead of renaming them to Rust "
        "convention. By default (FR-53) the emitted crate is renamed to "
        "idiomatic Rust -- functions and struct fields to snake_case, globals, "
        "statics, consts and enum variants to SCREAMING_SNAKE_CASE, and struct "
        "and enum types to UpperCamelCase -- so it compiles clean under the "
        "standard naming lints. This flag restores the historical verbatim "
        "spelling. A rename that would fold two distinct C names onto one Rust "
        "name is rejected with a located diagnostic; --preserve-c-names avoids "
        "it."),
    llvm::cl::init(false));

static llvm::cl::opt<bool> deferExternalsFlag(
    "defer-externals",
    llvm::cl::desc(
        "Deferred-externals import mode (FR-57a), the per-TU import mode for "
        "the FR-56/FR-57 shim path: a referenced external symbol that no "
        "imported translation unit defines becomes a declaration stub marked "
        "'emitrust.extern_decl' -- an undefined extern global materializes "
        "as a declaration-only emitrust.global, and a referenced body-less "
        "function keeps its declaration -- instead of failing the import. "
        "The emitted module carries those stubs as link-time obligations the "
        "FR-58 link step must resolve, so this mode is incompatible with "
        "direct crate emission: the Rust emitter refuses a module still "
        "carrying a stub with a located error. Off by default, in which case "
        "the compile is byte-identical to one built without this flag"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> incrementalFlag(
    "incremental",
    llvm::cl::desc(
        "Incremental crate output (FR-44, only valid with --emit=crate): "
        "IMPLIES --recover, so a project only partially inside the supported "
        "subset still yields a crate that BUILDS, and additionally writes two "
        "progress artifacts into the crate directory -- PORTING.md, a "
        "human-readable table of every project item with its porting status, "
        "blocker tag and source location, ranked by blocker; and "
        "emitrust-progress.json, the same data machine-readable with totals, "
        "so two runs can be diffed item by item. The item inventory is the "
        "FR-40 project item graph, so the denominator counts what the project "
        "HAS, not just what the importer reached. Without this flag the "
        "compile is byte-identical to one built without it"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> searchFlag(
    "search",
    llvm::cl::desc(
        "Frontier tree search (FR-43, only valid with --emit=crate "
        "--incremental, and implied by --emit=search): instead of keeping "
        "whatever the first recovering import happens to accept, search over "
        "SETS of admitted items for the largest subset that really imports. "
        "The root candidate is every item the FR-41 coloring calls Green or "
        "Yellow; each candidate is probed by a real recovering import, every "
        "rejection it reports is a fact the next candidate accounts for, and "
        "candidates are scored lexicographically by items emitted for real, "
        "then stub count, then representation cost. Bounded by "
        "--max-search-nodes and reproducible run to run. This matters most "
        "when a recovering import FAILS outright -- one item dropped can "
        "leave the project referencing a symbol nobody defines, which is a "
        "whole-program error no per-item recovery can undo -- where without "
        "it --incremental produces no crate at all"),
    llvm::cl::init(false));

static llvm::cl::opt<unsigned> maxSearchNodes(
    "max-search-nodes",
    llvm::cl::desc("Maximum number of candidate subsets the FR-43 search may "
                   "IMPORT (default 8). The import dominates the cost, so "
                   "this is the bound that bounds the run time; a project "
                   "whose coloring is exact converges after one"),
    llvm::cl::value_desc("n"), llvm::cl::init(8));

static llvm::cl::opt<std::string> searchTracePath(
    "search-trace",
    llvm::cl::desc("Write the FR-43 search trace to <path> ('-' means "
                   "stderr): one line per candidate probed, per fact learned, "
                   "per candidate pruned, then the winning item set and the "
                   "reason every other item is out. This is how \"why did it "
                   "stop there\" is answered. With --emit=search the trace is "
                   "the output and this flag is unnecessary"),
    llvm::cl::value_desc("path"), llvm::cl::init(""));

static llvm::cl::opt<bool> checkRangeRefinement(
    "check-range-refinement",
    llvm::cl::desc(
        "Run the emitrust-range-refinement-check verification pass on the "
        "pre-conversion stage of the pipeline (after lift-cf-to-scf and its "
        "canonicalize, i.e. on the exact input of convert-to-emitrust, the "
        "stage the checker analyzes by design): it clones that stage, "
        "converts the clone internally, and fails the compile with a "
        "located 'range refinement violation' diagnostic if any observable "
        "value's pre- and post-conversion integer ranges are disjoint (off "
        "by default)"),
    llvm::cl::init(false));

/// Prints one diagnostic (and its notes) to stderr as
/// `file:line:col: severity: message`, matching the format the lit tests
/// assert on. Diagnostics without a file location omit the prefix.
static void printDiagnostic(mlir::Diagnostic &diag) {
  auto printOne = [](mlir::Location loc, llvm::StringRef severity,
                     llvm::StringRef message) {
    if (auto fileLoc = llvm::dyn_cast<mlir::FileLineColLoc>(loc))
      llvm::errs() << fileLoc.getFilename().getValue() << ":"
                   << fileLoc.getLine() << ":" << fileLoc.getColumn() << ": ";
    llvm::errs() << severity << ": " << message << "\n";
  };
  llvm::StringRef severity = "error";
  switch (diag.getSeverity()) {
  case mlir::DiagnosticSeverity::Error:
    severity = "error";
    break;
  case mlir::DiagnosticSeverity::Warning:
    severity = "warning";
    break;
  case mlir::DiagnosticSeverity::Note:
    severity = "note";
    break;
  case mlir::DiagnosticSeverity::Remark:
    severity = "remark";
    break;
  }
  printOne(diag.getLocation(), severity, diag.str());
  for (mlir::Diagnostic &note : diag.getNotes())
    printOne(note.getLocation(), "note", note.str());
}

/// Runs the pinned lowering pipeline on `module`:
/// mem2reg -> canonicalize -> lift-cf-to-scf -> canonicalize ->
/// convert-to-emitrust. Pass verification catches any invalid intermediate
/// state; failures carry located diagnostics through the context.
///
/// With --check-range-refinement, the emitrust-range-refinement-check pass
/// is inserted right after the second canonicalize, i.e. immediately
/// before convert-to-emitrust — the exact "before" stage the checker's
/// primary mode is designed for (its internal clone pipeline runs only
/// convert-to-emitrust, so it must see post-lift IR: unlifted cf ops
/// cannot be legalized by the conversion alone). The pass is purely
/// observational — it clones the module and runs the conversion on the
/// clone internally — so the main pipeline continues unchanged after it;
/// a violation fails the compile with the pass's located diagnostic.
///
/// \param module the imported module to lower in place.
/// \returns success if every pass succeeded.
static mlir::LogicalResult runPipeline(mlir::ModuleOp module) {
  mlir::PassManager pm(module.getContext(),
                       mlir::ModuleOp::getOperationName());
  // Lower the high-level container ops (the FR-39 node pool) to the concrete
  // `[T;CAP]` array + cursor shape FIRST, before mem2reg promotes the cursor,
  // so all downstream lowering is identical to inlining the pool directly.
  pm.addPass(mlir::emitrust::createEmitRustLowerContainers());
  pm.addPass(mlir::createMem2Reg());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::createLiftControlFlowToSCFPass());
  pm.addPass(mlir::createCanonicalizerPass());
  if (checkRangeRefinement)
    pm.addPass(mlir::emitrust::createEmitRustRangeRefinementCheck());
  pm.addPass(mlir::emitrust::createConvertToEmitRust());
  // FR-52: strictly after the conversion, because the trait is expressed in
  // the EMITTED names and in the opaque string callees the conversion
  // produces. A no-op unless the importer marked an unresolved external,
  // which is why every existing crate stays byte-identical.
  pm.addPass(mlir::emitrust::createEmitRustLowerExternalRequirements());
  return pm.run(module);
}

/// FR-52: the unresolved-external policy implied by `--crate-type`.
///
/// The mapping is the whole decision, stated once:
///  - `bin` — REJECT. A binary crate's `fn main` is not generic and has no
///    caller to supply an impl, so a requirement it cannot satisfy is a
///    compile-time error, exactly as before. Emitting a `todo!()` default
///    impl instead would turn that into a RUNTIME panic and would let FR-44
///    score an unrunnable crate as fully ported — the differential oracle
///    this project rests on would stop being able to tell the difference.
///  - `auto` — REJECT for a module that defines `c_main` (which `auto` turns
///    into a binary crate, so the reasoning above applies verbatim), and
///    TRAIT for one that does not. The importer applies the same `c_main`
///    predicate `selectCrateType` does, so the two cannot disagree.
///  - `lib` — TRAIT unconditionally. The user has already said the output is
///    a library; a library's whole job is to be linked against something,
///    and `c_main` in it is an ordinary exported function that a caller can
///    instantiate like any other.
static mlir::emitrust::ExternalRequirements externalRequirementsPolicy() {
  switch (crateTypeOpt) {
  case emitrustcc::CrateTypeRequest::Bin:
    return mlir::emitrust::ExternalRequirements::Reject;
  case emitrustcc::CrateTypeRequest::Lib:
    return mlir::emitrust::ExternalRequirements::Trait;
  case emitrustcc::CrateTypeRequest::Auto:
    return mlir::emitrust::ExternalRequirements::TraitWhenLibrary;
  }
  llvm_unreachable("covered switch");
}

/// Writes `content` to `path` ('-' means stdout) atomically with respect to
/// failure: the file is only kept once the full content has been written.
///
/// \param path the output file path or '-'.
/// \param content the complete file contents.
/// \returns success if the file was written and kept.
static mlir::LogicalResult writeFile(llvm::StringRef path,
                                     llvm::StringRef content) {
  std::string errorMessage;
  std::unique_ptr<llvm::ToolOutputFile> output =
      mlir::openOutputFile(path, &errorMessage);
  if (!output) {
    llvm::errs() << "error: " << errorMessage << "\n";
    return mlir::failure();
  }
  output->os() << content;
  output->keep();
  return mlir::success();
}

/// Prints `module` as MLIR text to `path` ('-' means stdout).
///
/// \param module the module to print.
/// \param path the output file path or '-'.
/// \returns success if the file was written.
static mlir::LogicalResult writeModule(mlir::ModuleOp module,
                                       llvm::StringRef path) {
  std::string text;
  llvm::raw_string_ostream os(text);
  module.print(os);
  os << "\n";
  return writeFile(path, text);
}

/// Emits the cargo crate for `module` into the directory `outDir`: renders
/// `Cargo.toml` and the crate root first (pure, no partial output on a
/// translation failure), then creates the directories and writes both files.
///
/// FR-51: `type` decides the crate's shape and therefore the root's NAME —
/// `src/main.rs` for a binary crate, `src/lib.rs` for a library one. Both
/// files come from the pure renderers, so a library crate is not a special
/// case here beyond the file name.
///
/// \param module the fully converted module; must define `c_main` when
///        `type` is `CrateType::Bin`.
/// \param outDir the crate directory to create and populate.
/// \param crateName the sanitized cargo package name.
/// \param type the crate shape to emit.
/// \returns success if the whole crate was written.
static mlir::LogicalResult emitCrate(mlir::ModuleOp module,
                                     llvm::StringRef outDir,
                                     llvm::StringRef crateName,
                                     emitrustcc::CrateType type) {
  mlir::FailureOr<std::string> rootRs =
      emitrustcc::renderCrateRoot(module, type);
  if (mlir::failed(rootRs))
    return mlir::failure();
  std::string cargoToml = emitrustcc::renderCargoToml(crateName, type);

  llvm::SmallString<256> srcDir(outDir);
  llvm::sys::path::append(srcDir, "src");
  if (std::error_code ec = llvm::sys::fs::create_directories(srcDir)) {
    llvm::errs() << "error: cannot create directory '" << srcDir
                 << "': " << ec.message() << "\n";
    return mlir::failure();
  }

  llvm::SmallString<256> tomlPath(outDir);
  llvm::sys::path::append(tomlPath, "Cargo.toml");
  llvm::SmallString<256> rootPath(srcDir);
  llvm::sys::path::append(rootPath, emitrustcc::crateRootFileName(type));
  if (mlir::failed(writeFile(tomlPath, cargoToml)))
    return mlir::failure();
  return writeFile(rootPath, *rootRs);
}

/// Reports the one invalid FR-51 crate shape: a BINARY crate for a module
/// that defines no `c_main`.
///
/// `--crate-type=auto` can never reach this — it selects `Lib` in exactly
/// that case — so the diagnostic is only ever produced by an explicit
/// `--crate-type=bin`. It is deliberately kept as an error rather than being
/// silently downgraded to a library: the user asked for an executable, and a
/// binary crate with no entry point does not link, so quietly handing back
/// something else would answer a question that was not asked.
///
/// \param module the fully converted module.
/// \param type the selected crate shape.
/// \returns success unless a binary crate was requested without a `main`.
static mlir::LogicalResult diagnoseCrateType(mlir::ModuleOp module,
                                             emitrustcc::CrateType type) {
  if (type != emitrustcc::CrateType::Bin || emitrustcc::hasCMain(module))
    return mlir::success();
  module.emitError()
      << "cannot emit a binary crate: the input does not define a 'main' "
         "function (imported as 'c_main'). Drop --crate-type=bin to emit a "
         "library crate instead";
  return mlir::failure();
}

/// Writes the two FR-44 progress artifacts into the crate directory that
/// `emitCrate` has already created: `PORTING.md` and
/// `emitrust-progress.json`. Both are rendered by the pure functions in
/// ProgressReport.h; this function only names the files and writes them.
///
/// They live INSIDE the crate directory on purpose: the report describes that
/// exact crate, and cargo ignores unknown files at a package root, so the
/// crate still builds with them present.
///
/// \param outDir the crate directory written by emitCrate.
/// \param report the joined per-item report.
/// \returns success if both files were written.
static mlir::LogicalResult
writeProgressArtifacts(llvm::StringRef outDir,
                       const emitrustcc::ProgressReport &report) {
  llvm::SmallString<256> portingPath(outDir);
  llvm::sys::path::append(portingPath, "PORTING.md");
  llvm::SmallString<256> jsonPath(outDir);
  llvm::sys::path::append(jsonPath, "emitrust-progress.json");
  if (mlir::failed(
          writeFile(portingPath, emitrustcc::renderPortingMarkdown(report))))
    return mlir::failure();
  return writeFile(jsonPath, emitrustcc::renderProgressJson(report));
}

/// Runs `cargo build --release --offline` on the crate in `outDir` by
/// pointing cargo at its manifest (equivalent to running in the crate
/// directory: the target directory lands in `<outDir>/target`). Cargo is
/// located on PATH; a clear error is produced when it is absent or the
/// build fails.
///
/// \param outDir the crate directory written by emitCrate.
/// \returns success if the build completed with exit code zero.
static mlir::LogicalResult buildCrate(llvm::StringRef outDir) {
  llvm::ErrorOr<std::string> cargo = llvm::sys::findProgramByName("cargo");
  if (!cargo) {
    llvm::errs() << "error: cannot find 'cargo' on PATH: "
                 << cargo.getError().message() << "\n";
    return mlir::failure();
  }

  llvm::SmallString<256> manifestPath(outDir);
  llvm::sys::path::append(manifestPath, "Cargo.toml");
  llvm::SmallVector<llvm::StringRef, 8> args = {
      "cargo",     "build",          "--release",
      "--offline", "--manifest-path", manifestPath};

  std::string errMsg;
  bool executionFailed = false;
  int result = llvm::sys::ExecuteAndWait(*cargo, args, /*Env=*/std::nullopt,
                                         /*Redirects=*/{}, /*SecondsToWait=*/0,
                                         /*MemoryLimit=*/0, &errMsg,
                                         &executionFailed);
  if (executionFailed || result != 0) {
    llvm::errs() << "error: cargo build failed";
    if (result > 0)
      llvm::errs() << " with exit code " << result;
    if (!errMsg.empty())
      llvm::errs() << ": " << errMsg;
    llvm::errs() << "\n";
    return mlir::failure();
  }
  return mlir::success();
}

//===----------------------------------------------------------------------===//
// FR-58 -- the link step's imperative shell
//===----------------------------------------------------------------------===//

/// One parsed link-line shard plus the name that attributes it in
/// link-step reporting: the input path itself, or `<archive>(<member>)`
/// for an expanded archive member.
struct LoadedShard {
  mlir::OwningOpRef<mlir::ModuleOp> module;
  std::string name;
};

/// Loads, extracts, and parses the link-line `inputs` into per-TU shard
/// modules, in link-line order (FR-58 slices 1 and 2, `--link`). The
/// functional core is `emitrustcc::findShardPayload` (payload location
/// inside one input) and `emitrustcc::findArchivePayloads` (static-archive
/// expansion); this function owns the file reads and the parse. Each input
/// is an object file whose `.emitrust` section carries the shard, an
/// object falling back to its `<object>.emitrust.mlirbc` sidecar, a static
/// archive whose members are expanded in archive order as if listed loose
/// at the archive's position (a payload-less member warns and is skipped —
/// members have no sidecar to fall back to), or a shard file named
/// directly — `parseSourceFile` handles MLIR bytecode and text alike.
/// Every failure path has printed a diagnostic by the time this returns
/// false.
///
/// \param inputs the link-line object/archive/shard paths, in link order.
/// \param context the context to parse in; the EmitRust dialect is loaded
///        here because no import will do it.
/// \param shards receives one named shard per payload, in link-line order.
/// \returns true when every input yielded its shards.
static bool loadLinkShards(llvm::ArrayRef<std::string> inputs,
                           mlir::MLIRContext &context,
                           llvm::SmallVectorImpl<LoadedShard> &shards) {
  context.loadDialect<mlir::emitrust::EmitRustDialect>();
  // Parses one shard payload — copied into its own buffer named
  // `bufferName`, because payloads point into files that die before the
  // parsed module's string attrs and parser diagnostics do — and appends
  // it to `shards`.
  auto parseShard = [&](llvm::StringRef payload,
                        const llvm::Twine &bufferName) -> mlir::LogicalResult {
    std::string name = bufferName.str();
    llvm::SourceMgr sourceMgr;
    sourceMgr.AddNewSourceBuffer(
        llvm::MemoryBuffer::getMemBufferCopy(payload, name), llvm::SMLoc());
    mlir::ParserConfig parserConfig(&context);
    mlir::OwningOpRef<mlir::ModuleOp> shard =
        mlir::parseSourceFile<mlir::ModuleOp>(sourceMgr, parserConfig);
    if (!shard)
      return mlir::failure();
    shards.push_back(LoadedShard{std::move(shard), std::move(name)});
    return mlir::success();
  };
  for (const std::string &path : inputs) {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> file =
        llvm::MemoryBuffer::getFile(path);
    if (!file) {
      llvm::errs() << "error: cannot read '" << path
                   << "': " << file.getError().message() << "\n";
      return false;
    }
    if (emitrustcc::isStaticArchive(**file)) {
      // FR-58 slice 2: expand the archive in place, members in archive
      // order, exactly as if they had been listed here on the link line.
      std::string archiveError;
      mlir::FailureOr<emitrustcc::ArchivePayloads> expanded =
          emitrustcc::findArchivePayloads(**file, archiveError);
      if (mlir::failed(expanded)) {
        llvm::errs() << "error: cannot expand archive '" << path
                     << "': " << archiveError << "\n";
        return false;
      }
      for (const std::string &member : expanded->skippedMembers)
        llvm::errs() << "warning: archive member '" << member << "' of '"
                     << path << "' has no .emitrust payload; skipped\n";
      for (const emitrustcc::ArchiveMemberPayload &member :
           expanded->payloads)
        if (mlir::failed(parseShard(member.payload,
                                    path + "(" + member.memberName + ")")))
          return false;
      continue;
    }
    std::string sectionError;
    mlir::FailureOr<std::optional<llvm::StringRef>> payload =
        emitrustcc::findShardPayload(**file, sectionError);
    if (mlir::failed(payload)) {
      llvm::errs() << "error: cannot read the .emitrust section of '" << path
                   << "': " << sectionError << "\n";
      return false;
    }
    if (*payload) {
      if (mlir::failed(parseShard(**payload, path)))
        return false;
      continue;
    }
    // FR-57b keeps the sidecar as the non-ELF fallback; honoring it here
    // is what makes the pair round-trip without objcopy cooperation.
    std::string sidecar = path + ".emitrust.mlirbc";
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> side =
        llvm::MemoryBuffer::getFile(sidecar);
    if (!side) {
      llvm::errs() << "error: object '" << path
                   << "' has no .emitrust section and its sidecar '"
                   << sidecar
                   << "' cannot be read: " << side.getError().message()
                   << "\n";
      return false;
    }
    if (mlir::failed(parseShard((*side)->getBuffer(), sidecar)))
      return false;
  }
  return true;
}

/// FR-58 selective re-import of fact-starved items. A solo shard whose
/// ledger carries a MEASURED fact-starvation rejection (an extern pointer
/// global the solo import could not type; see design.md FR-58 SPIKE 2 and
/// the re-import spike) is grouped with every shard whose ITEM GRAPH
/// defines the missing global — the graph records the definition even when
/// the defining shard's module never materialized the item — and each
/// group's member SOURCES (recorded in the artifacts' `emitrust.source`)
/// are re-imported JOINTLY at link time through the same recover +
/// defer-externals import and pinned pipeline the shim runs. The group
/// module then stands in for its member shards, with an ordinal map
/// carrying its members' link-line positions so the merge renames its
/// group-relative `tu<k>_` tags onto the ordinals the whole-project joint
/// import would have used.
///
/// Every failure DEGRADES rather than fails: a shard with no recorded
/// source, group members whose recorded import args differ (a joint
/// `importCProject` applies ONE arg list to all TUs), or a re-import/
/// pipeline failure each warn and leave the original shards — the link
/// then produces exactly what it produced before selective re-import
/// existed (ledgered stubs that fail loudly if executed).
///
/// \param shards the loaded link-line shards; group members are replaced
///        in place (the group module lands at its first member's position,
///        later members are dropped).
/// \param context the context every module lives in.
/// \param ordinalMaps receives one tag-ordinal map per surviving shard,
///        parallel to the updated `shards`.
static void reimportFactStarvedGroups(
    llvm::SmallVectorImpl<LoadedShard> &shards, mlir::MLIRContext &context,
    llvm::SmallVectorImpl<llvm::SmallVector<unsigned>> &ordinalMaps) {
  size_t count = shards.size();
  llvm::SmallVector<unsigned> parent(count);
  for (unsigned i = 0; i < count; ++i)
    parent[i] = i;
  auto findRoot = [&](unsigned x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };

  // Indexed definer discovery (measured at 10^3 shards, design.md FR-58):
  // one pass over every shard's graph text builds the emitted-symbol ->
  // defining-shards map, so each starved name is a hash lookup instead of
  // a text scan of every shard's whole graph.
  llvm::StringMap<llvm::SmallVector<unsigned, 1>> globalDefiners;
  for (unsigned j = 0; j < count; ++j)
    if (std::optional<llvm::StringRef> graph =
            mlir::emitrust::getShardItemGraph(*shards[j].module))
      for (llvm::StringRef symbol : emitrustcc::itemGraphGlobalDefs(*graph))
        globalDefiners[symbol].push_back(j);

  bool anyGroup = false;
  for (unsigned i = 0; i < count; ++i) {
    for (const mlir::emitrust::RejectedItem &item :
         mlir::emitrust::getShardRejections(*shards[i].module)) {
      if (!emitrustcc::isFactStarvedDiagnostic(item.diagnostic))
        continue;
      for (const std::string &name :
           emitrustcc::factStarvedObjectNames(item.symbol, item.diagnostic)) {
        auto it = globalDefiners.find(mlir::emitrust::globalRustName(name));
        if (it == globalDefiners.end())
          continue;
        for (unsigned j : it->second) {
          if (j == i)
            continue;
          parent[findRoot(i)] = findRoot(j);
          anyGroup = true;
        }
      }
    }
  }

  // Second detection axis: signature-starved extern declarations — a
  // body-less `extern_decl` function whose declared type disagrees with
  // the defining shard's. The declaring shard shaped its call sites from
  // the bare prototype (its ledger is silent), so only this whole-program
  // comparison can see the divergence; dropping the declaration for the
  // definition, as the merge does for matching signatures, would leave
  // those call sites shaped for a type the definition does not have.
  {
    llvm::SmallVector<mlir::ModuleOp> modules;
    modules.reserve(count);
    for (LoadedShard &shard : shards)
      modules.push_back(*shard.module);
    for (const emitrustcc::SignatureStarvation &starvation :
         emitrustcc::findSignatureStarvedDecls(modules)) {
      parent[findRoot(starvation.declShard)] = findRoot(starvation.defShard);
      anyGroup = true;
    }
  }

  if (!anyGroup) {
    for (unsigned i = 0; i < count; ++i)
      ordinalMaps.push_back({i});
    return;
  }

  // Root -> sorted member positions; only multi-member components form
  // re-import groups (a starved shard with no discovered definer has no
  // facts to gain).
  llvm::SmallVector<llvm::SmallVector<unsigned>> members(count);
  for (unsigned i = 0; i < count; ++i)
    members[findRoot(i)].push_back(i);

  // Attempt each group's joint re-import; a failed group degrades to its
  // original member shards.
  llvm::SmallVector<std::optional<LoadedShard>> groupModule(count);
  llvm::SmallVector<bool> grouped(count, false);
  for (unsigned root = 0; root < count; ++root) {
    llvm::SmallVector<unsigned> &group = members[root];
    if (group.size() < 2)
      continue;
    llvm::sort(group);

    std::vector<std::string> paths;
    std::vector<std::string> args;
    std::string groupLabel;
    bool viable = true;
    for (unsigned position : group) {
      std::optional<mlir::emitrust::ShardSource> source =
          mlir::emitrust::getShardSource(*shards[position].module);
      if (!source) {
        llvm::errs() << "warning: cannot re-import fact-starved group: '"
                     << shards[position].name
                     << "' records no source facts; keeping its stubs\n";
        viable = false;
        break;
      }
      if (paths.empty()) {
        args = source->args;
      } else if (args != source->args) {
        llvm::errs() << "warning: cannot re-import fact-starved group: "
                        "members' recorded import args differ ('"
                     << shards[position].name << "'); keeping the stubs\n";
        viable = false;
        break;
      }
      paths.push_back(source->path);
      groupLabel += (groupLabel.empty() ? "" : " + ") + source->path;
    }
    if (!viable)
      continue;

    llvm::errs() << "link-time re-import: " << groupLabel << "\n";
    mlir::emitrust::RejectionLedger reimportLedger;
    mlir::emitrust::ImportOptions options;
    options.recover = true;
    options.deferExternals = true;
    options.ledger = &reimportLedger;
    mlir::OwningOpRef<mlir::ModuleOp> module =
        mlir::emitrust::importCProject(paths, args, options, context);
    if (!module || mlir::failed(runPipeline(*module))) {
      llvm::errs() << "warning: link-time re-import of " << groupLabel
                   << " failed; keeping the original shards\n";
      continue;
    }
    if (!reimportLedger.empty()) {
      llvm::errs() << "link-time re-import of " << groupLabel << ": ";
      reimportLedger.printSummary(llvm::errs());
    }
    groupModule[root] =
        LoadedShard{std::move(module), ("re-import(" + groupLabel + ")")};
    for (unsigned position : group)
      grouped[position] = true;
  }

  llvm::SmallVector<LoadedShard> result;
  for (unsigned position = 0; position < count; ++position) {
    unsigned root = findRoot(position);
    if (grouped[position]) {
      // The group module stands at its FIRST member's position and carries
      // its members' link-line positions as the tag-ordinal map.
      if (position == members[root].front()) {
        result.push_back(std::move(*groupModule[root]));
        ordinalMaps.push_back(members[root]);
      }
      continue;
    }
    result.push_back(std::move(shards[position]));
    ordinalMaps.push_back({position});
  }
  shards = std::move(result);
}

/// `loadLinkShards` + FR-57d surfacing + FR-58 selective re-import + the
/// FR-58 merge: the full `--link` module path. Before the shards are
/// consumed by the merge, each shard's rejection-ledger entries (recorded
/// by the shim's recovering import and carried in the artifact, see
/// EmitRust/ShardMetadata.h) are printed in the same summary format the
/// joint import uses, prefixed with the shard they came from; the
/// fact-starved subset then drives `reimportFactStarvedGroups`. The merge
/// itself strips the metadata, keeping the merged module byte-comparable
/// to the joint import's.
///
/// \param inputs the link-line object/archive/shard paths, in link order.
/// \param context the context to parse and merge in.
/// \returns the merged, verified whole-program module, or null after a
///          printed diagnostic.
static mlir::OwningOpRef<mlir::ModuleOp>
loadAndMergeShards(llvm::ArrayRef<std::string> inputs,
                   mlir::MLIRContext &context) {
  llvm::SmallVector<LoadedShard> shards;
  if (!loadLinkShards(inputs, context, shards))
    return nullptr;
  for (LoadedShard &shard : shards) {
    llvm::SmallVector<mlir::emitrust::RejectedItem> rejections =
        mlir::emitrust::getShardRejections(*shard.module);
    if (!rejections.empty()) {
      mlir::emitrust::RejectionLedger shardLedger;
      for (mlir::emitrust::RejectedItem &item : rejections)
        shardLedger.record(std::move(item));
      llvm::errs() << "shard '" << shard.name << "': ";
      shardLedger.printSummary(llvm::errs());
    }
  }
  llvm::SmallVector<llvm::SmallVector<unsigned>> ordinalMaps;
  reimportFactStarvedGroups(shards, context, ordinalMaps);
  llvm::SmallVector<mlir::OwningOpRef<mlir::ModuleOp>> modules;
  modules.reserve(shards.size());
  for (LoadedShard &shard : shards)
    modules.push_back(std::move(shard.module));
  mlir::FailureOr<mlir::OwningOpRef<mlir::ModuleOp>> merged =
      emitrustcc::mergeLinkShards(modules, ordinalMaps);
  if (mlir::failed(merged))
    return nullptr;
  return std::move(*merged);
}

/// `--link --emit=item-graph`: dumps each shard's STORED item-graph text
/// (FR-57d, `emitrust.item_graph`), one `shard <i> '<name>'` header per
/// shard in link-line order, from the artifacts alone — no C is parsed.
/// Deliberately NOT a merged graph: merging the shards' graphs
/// (alpha-renaming `tu0_` tags, deduplicating shared nodes) is FR-58's own
/// open item, and printing a naive concatenation AS IF merged would be
/// silently wrong; the per-shard dump is the honest surface the artifacts
/// support today. A shard carrying no graph (an artifact from before
/// metadata support) is a located error, not an empty section — the caller
/// asked for facts the artifact cannot supply.
///
/// \param inputs the link-line object/archive/shard paths, in link order.
/// \param outputPath the `-o` destination (`-` for stdout).
/// \returns the process exit code.
static int emitLinkShardItemGraphs(llvm::ArrayRef<std::string> inputs,
                                   llvm::StringRef outputPath) {
  mlir::MLIRContext context;
  context.getDiagEngine().registerHandler(
      [](mlir::Diagnostic &diag) { printDiagnostic(diag); });
  llvm::SmallVector<LoadedShard> shards;
  if (!loadLinkShards(inputs, context, shards))
    return 1;
  std::string text;
  llvm::raw_string_ostream os(text);
  for (auto [index, shard] : llvm::enumerate(shards)) {
    std::optional<llvm::StringRef> graph =
        mlir::emitrust::getShardItemGraph(*shard.module);
    if (!graph) {
      llvm::errs() << "error: shard '" << shard.name
                   << "' carries no item-graph metadata (artifact predates "
                      "FR-57d?)\n";
      return 1;
    }
    // A present-but-empty graph is a TU that contributes no items (FR-56's
    // whole-TU target/ABI rejection): the header still prints, the body is
    // legitimately empty.
    os << "shard " << index << " '" << shard.name << "'\n" << *graph;
  }
  return mlir::failed(writeFile(outputPath, text)) ? 1 : 0;
}

//===----------------------------------------------------------------------===//
// FR-43 -- the frontier search's probe
//===----------------------------------------------------------------------===//

/// Runs ONE candidate subset through the real compiler and reports what came
/// out: the imperative shell of FR-43, whose functional core is
/// `mlir::emitrust::frontierSearch`.
///
/// Everything expensive about the search is here, and it is deliberately the
/// SAME path an ordinary `--emit=crate --incremental` compile takes — a
/// recovering `importCProject` with the state's complement excluded, then the
/// pinned lowering pipeline, then FR-44's join of graph, ledger and emitted
/// symbol table. Scoring a candidate by anything cheaper would score a
/// different compiler than the one that will produce the crate.
///
/// Three shell concerns live here and nowhere else:
///
///  - A FRESH `MLIRContext` per probe. A module is owned by its context and a
///    probe may leave a half-built one behind; a fresh context makes each
///    attempt independent by construction rather than by cleanup.
///  - DIAGNOSTIC CAPTURE. A probe is a hypothesis, not a compile: its
///    warnings are noise (the corpus's projects produce dozens each) and its
///    error is data. The handler swallows both and keeps the first error,
///    which is the one that killed the import and therefore the one the
///    search must attribute.
///  - The MISSING-SYMBOL parse. `finalizeProject`'s "'g' is referenced but
///    not defined in any translation unit" is the canonical whole-program
///    failure a per-item recovery cannot undo, and the symbol it names is the
///    single most useful fact the search can get, so it is lifted out of the
///    message text into `ProbeOutcome::failureSymbol`.
///
/// \param inputs the project analyses, parsed once for the whole search.
/// \param paths the source files.
/// \param extra the clang arguments.
/// \param crateName the sanitized package name, for the report join only.
/// \param state the candidate subset to probe.
/// \returns what the attempt produced.
static mlir::emitrust::ProbeOutcome
probeSearchState(const mlir::emitrust::SearchInputs &inputs,
                 llvm::ArrayRef<std::string> paths,
                 llvm::ArrayRef<std::string> extra, llvm::StringRef crateName,
                 const mlir::emitrust::SearchState &state) {
  mlir::emitrust::ProbeOutcome outcome;

  mlir::MLIRContext context;
  bool haveFailure = false;
  context.getDiagEngine().registerHandler([&](mlir::Diagnostic &diag) {
    if (diag.getSeverity() != mlir::DiagnosticSeverity::Error || haveFailure)
      return;
    haveFailure = true;
    outcome.failure = diag.str();
    if (auto fileLoc = llvm::dyn_cast<mlir::FileLineColLoc>(diag.getLocation())) {
      outcome.failureFile = fileLoc.getFilename().getValue().str();
      outcome.failureLine = fileLoc.getLine();
    }
    // `unsupported: function 'f' is referenced but not defined in any
    // translation unit` and its extern-global twin: the quoted run between
    // the last `'` pair before the fixed suffix is the missing symbol.
    static constexpr llvm::StringLiteral kSuffix =
        "' is referenced but not defined in any translation unit";
    llvm::StringRef message(outcome.failure);
    if (size_t end = message.find(kSuffix); end != llvm::StringRef::npos) {
      llvm::StringRef head = message.take_front(end);
      if (size_t start = head.rfind('\''); start != llvm::StringRef::npos)
        outcome.failureSymbol = head.drop_front(start + 1).str();
    }
  });
  context.loadDialect<mlir::scf::SCFDialect, mlir::ub::UBDialect>();

  mlir::emitrust::RejectionLedger ledger;
  mlir::emitrust::ImportOptions options;
  options.recover = true;
  options.ledger = &ledger;
  options.compilationDatabasePath = compilationDatabasePath;
  options.excludedItems = mlir::emitrust::excludedItemsFor(inputs.graph, state);
  // FR-52: the probe must compile the SAME project the final run will, or it
  // would score a compiler that is not the one producing the crate — which is
  // the whole reason the probe is a real import in the first place.
  options.externalRequirements = externalRequirementsPolicy();
  // FR-57a: same same-project reasoning as the FR-52 policy above.
  options.deferExternals = deferExternalsFlag;

  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::emitrust::importCProject(paths, extra, options, context);
  // The rejections are facts whether or not the import survived: an item the
  // coloring called Green and the importer refused is a learned fact even on
  // a run that later died for an unrelated reason.
  for (const mlir::emitrust::RejectedItem &item : ledger.getItems())
    outcome.rejections.push_back(
        {item.symbol, item.stubbed, item.blockerTag});
  if (!module) {
    if (outcome.failure.empty())
      outcome.failure = "the import failed without a located diagnostic";
    return outcome;
  }
  // Lowering is part of the probe because it is part of producing a crate: a
  // subset that imports but does not lower yields no output, and scoring it
  // as if it did would send the search down a branch that cannot pay off.
  if (mlir::failed(runPipeline(*module))) {
    if (outcome.failure.empty())
      outcome.failure = "the lowering pipeline failed";
    return outcome;
  }

  llvm::StringSet<> emitted = emitrustcc::collectEmittedSymbols(*module);
  emitrustcc::ProgressReport report = emitrustcc::buildProgressReport(
      crateName, &inputs.graph, &inputs.coloring, ledger.getItems(), emitted);
  outcome.imported = true;
  outcome.ported = report.count(emitrustcc::ItemStatus::Ported);
  outcome.stubbed = report.count(emitrustcc::ItemStatus::Stubbed);
  outcome.dropped = report.count(emitrustcc::ItemStatus::Dropped);
  return outcome;
}

/// The sanitized cargo package name for this invocation: `--crate-name` when
/// given; otherwise, for a single input its stem (historical behavior), and
/// for several inputs the `-o` crate-directory stem (the input stems are
/// ambiguous). A `--compdb` run with no positional inputs takes the same
/// crate-directory stem: the database's file list is a project, not one
/// nameable input.
static std::string deducedCrateName(llvm::ArrayRef<std::string> inputs) {
  llvm::StringRef stem = !crateNameOpt.empty() ? llvm::StringRef(crateNameOpt)
                         : inputs.size() == 1
                             ? llvm::sys::path::stem(inputs.front())
                             : llvm::sys::path::stem(outputPath);
  return emitrustcc::sanitizeCrateName(stem);
}

/// Collects the `-I`, `-isystem`, and `--extra-arg` options into one clang
/// argument list, interleaved by command-line position so the include search
/// order matches what the user wrote.
static std::vector<std::string> collectExtraClangArgs() {
  std::vector<std::pair<unsigned, std::vector<std::string>>> items;
  for (unsigned i = 0, e = includeDirs.size(); i != e; ++i)
    items.push_back({includeDirs.getPosition(i), {"-I" + includeDirs[i]}});
  for (unsigned i = 0, e = systemIncludeDirs.size(); i != e; ++i)
    items.push_back(
        {systemIncludeDirs.getPosition(i), {"-isystem", systemIncludeDirs[i]}});
  for (unsigned i = 0, e = extraArgs.size(); i != e; ++i)
    items.push_back({extraArgs.getPosition(i), {extraArgs[i]}});
  llvm::stable_sort(items, [](const auto &a, const auto &b) {
    return a.first < b.first;
  });
  std::vector<std::string> args;
  for (const auto &item : items)
    args.insert(args.end(), item.second.begin(), item.second.end());
  return args;
}

//===----------------------------------------------------------------------===//
// FR-59 -- workspace partitioning
//===----------------------------------------------------------------------===//

/// Parses the `--partition-map` file (when given) into (prefix, crate)
/// override entries, crate names sanitized. Shared by the FR-59 workspace
/// path and FR-60's ratchet (whose manifest records the partition facts).
///
/// \param overrides receives the entries, in file order.
/// \returns true on success; false after a printed error.
static bool loadPartitionOverrides(
    std::vector<std::pair<std::string, std::string>> &overrides) {
  if (partitionMapPath.empty())
    return true;
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> map =
      llvm::MemoryBuffer::getFile(partitionMapPath);
  if (!map) {
    llvm::errs() << "error: cannot read --partition-map '" << partitionMapPath
                 << "': " << map.getError().message() << "\n";
    return false;
  }
  for (llvm::StringRef line : llvm::split((*map)->getBuffer(), '\n')) {
    line = line.trim();
    if (line.empty() || line.starts_with("#"))
      continue;
    auto [prefix, crate] = line.split(' ');
    crate = crate.trim();
    if (prefix.empty() || crate.empty()) {
      llvm::errs() << "error: malformed --partition-map line: '" << line
                   << "' (expected '<path-prefix> <crate-name>')\n";
      return false;
    }
    overrides.emplace_back(prefix.str(), emitrustcc::sanitizeCrateName(crate));
  }
  return true;
}

/// `--link --partition --emit=crate`: the full workspace path. Loads and
/// surfaces the shards exactly like `loadAndMergeShards`, HARVESTS each
/// link-line position's partition facts (source path, item-graph text)
/// BEFORE the selective re-import replaces group members (the returned
/// ordinal maps then say which positions each surviving unit covers), plans
/// the workspace with the pure `planPartition`, merges with per-op unit
/// attribution, SPLITS the merged module into one module per crate
/// (`emitrust.use`/`emitrust.verbatim` header ops are CLONED into every
/// member — a member gets at most an allowed unused import, never a missing
/// one), and writes the workspace: a virtual root manifest plus one member
/// directory per crate, the binary member last touched by `--build` through
/// the ordinary root-manifest cargo invocation.
///
/// \param inputs the link-line object/archive/shard paths, in link order.
/// \param context the context to parse, merge, and split in.
/// \returns the process exit code.
static int emitPartitionedWorkspace(llvm::ArrayRef<std::string> inputs,
                                    mlir::MLIRContext &context) {
  llvm::SmallVector<LoadedShard> shards;
  if (!loadLinkShards(inputs, context, shards))
    return 1;
  for (LoadedShard &shard : shards) {
    llvm::SmallVector<mlir::emitrust::RejectedItem> rejections =
        mlir::emitrust::getShardRejections(*shard.module);
    if (!rejections.empty()) {
      mlir::emitrust::RejectionLedger shardLedger;
      for (mlir::emitrust::RejectedItem &item : rejections)
        shardLedger.record(std::move(item));
      llvm::errs() << "shard '" << shard.name << "': ";
      shardLedger.printSummary(llvm::errs());
    }
  }

  // Harvest per-position partition facts BEFORE the re-import replaces
  // group members; a partition without source facts has no directory rule
  // to apply, so an old artifact is a clean error here (partitioning was
  // explicitly requested).
  size_t positions = shards.size();
  llvm::SmallVector<std::string> positionSource(positions);
  llvm::SmallVector<std::string> positionGraph(positions);
  for (auto [i, shard] : llvm::enumerate(shards)) {
    std::optional<mlir::emitrust::ShardSource> source =
        mlir::emitrust::getShardSource(*shard.module);
    if (!source) {
      llvm::errs() << "error: cannot partition: shard '" << shard.name
                   << "' records no source facts (artifact predates "
                      "FR-58?)\n";
      return 1;
    }
    positionSource[i] = source->path;
    if (std::optional<llvm::StringRef> graph =
            mlir::emitrust::getShardItemGraph(*shard.module))
      positionGraph[i] = graph->str();
  }

  llvm::SmallVector<llvm::SmallVector<unsigned>> ordinalMaps;
  reimportFactStarvedGroups(shards, context, ordinalMaps);

  // Units from the post-re-import shards; each unit's member positions come
  // from its ordinal map. Impl blocks are read off the MODULE (the graph
  // does not model methods) so the orphan rule can condense them to their
  // type's crate.
  llvm::SmallVector<emitrustcc::PartitionUnit> units(shards.size());
  for (auto [k, shard] : llvm::enumerate(shards)) {
    for (unsigned position : ordinalMaps[k]) {
      units[k].sourcePaths.push_back(positionSource[position]);
      units[k].graphTexts.push_back(positionGraph[position]);
    }
    for (mlir::Operation &op : shard.module->getBody()->getOperations())
      if (auto impl = llvm::dyn_cast<mlir::emitrust::ImplOp>(&op))
        units[k].implTypes.push_back(impl.getStructName().str());
  }

  std::vector<std::pair<std::string, std::string>> overrides;
  if (!loadPartitionOverrides(overrides))
    return 1;

  std::string binCrateName = deducedCrateName(inputs);
  emitrustcc::PartitionPlan plan =
      emitrustcc::planPartition(units, binCrateName, overrides);
  for (const std::string &note : plan.notes)
    llvm::errs() << "warning: workspace partition: " << note << "\n";

  // Per-op unit attribution, recorded before the merge splices everything
  // into one module (op pointers are stable across the splice; erased ops
  // are simply never looked up again).
  llvm::DenseMap<mlir::Operation *, unsigned> opUnit;
  for (auto [k, shard] : llvm::enumerate(shards))
    for (mlir::Operation &op : shard.module->getBody()->getOperations())
      opUnit[&op] = static_cast<unsigned>(k);

  llvm::SmallVector<mlir::OwningOpRef<mlir::ModuleOp>> modules;
  modules.reserve(shards.size());
  for (LoadedShard &shard : shards)
    modules.push_back(std::move(shard.module));
  mlir::FailureOr<mlir::OwningOpRef<mlir::ModuleOp>> merged =
      emitrustcc::mergeLinkShards(modules, ordinalMaps);
  if (mlir::failed(merged))
    return 1;

  // Split the merged module into one module per crate: header ops
  // (`emitrust.use`/`emitrust.verbatim`) are cloned into every crate in
  // their merged order, every other op moves to its unit's crate, and the
  // relative item order within each crate is the merged order restricted
  // to it.
  llvm::SmallVector<mlir::OwningOpRef<mlir::ModuleOp>> crateModules;
  for (size_t i = 0; i < plan.crates.size(); ++i)
    crateModules.push_back(
        mlir::ModuleOp::create(mlir::UnknownLoc::get(&context)));
  llvm::SmallVector<mlir::Operation *> mergedOps;
  for (mlir::Operation &op : (*merged)->getBody()->getOperations())
    mergedOps.push_back(&op);
  for (mlir::Operation *op : mergedOps) {
    if (llvm::isa<mlir::emitrust::UseOp, mlir::emitrust::VerbatimOp>(op)) {
      mlir::OpBuilder builder(&context);
      for (mlir::OwningOpRef<mlir::ModuleOp> &crateModule : crateModules) {
        builder.setInsertionPointToEnd(crateModule->getBody());
        builder.clone(*op);
      }
      op->erase();
      continue;
    }
    auto it = opUnit.find(op);
    if (it == opUnit.end()) {
      op->emitError() << "internal error: merged op has no shard attribution";
      return 1;
    }
    mlir::ModuleOp target = *crateModules[plan.unitCrate[it->second]];
    op->moveBefore(target.getBody(), target.getBody()->end());
  }
  for (mlir::OwningOpRef<mlir::ModuleOp> &crateModule : crateModules)
    if (mlir::failed(mlir::verify(*crateModule)))
      return 1;

  // Write the workspace.
  if (std::error_code ec = llvm::sys::fs::create_directories(outputPath)) {
    llvm::errs() << "error: cannot create directory '" << outputPath
                 << "': " << ec.message() << "\n";
    return 1;
  }
  llvm::SmallVector<std::string> memberNames;
  for (const emitrustcc::PartitionCrate &crate : plan.crates)
    memberNames.push_back(crate.name);
  llvm::SmallString<256> rootToml(outputPath);
  llvm::sys::path::append(rootToml, "Cargo.toml");
  if (mlir::failed(
          writeFile(rootToml, emitrustcc::renderWorkspaceToml(memberNames))))
    return 1;

  for (auto [index, crate] : llvm::enumerate(plan.crates)) {
    mlir::ModuleOp module = *crateModules[index];
    emitrustcc::CrateType type =
        crate.isBin ? emitrustcc::CrateType::Bin : emitrustcc::CrateType::Lib;
    if (crate.isBin && !emitrustcc::hasCMain(module)) {
      module.emitError() << "internal error: the binary member '"
                         << crate.name << "' lost its 'c_main'";
      return 1;
    }
    llvm::SmallVector<std::string> depNames;
    for (unsigned dep : crate.deps)
      depNames.push_back(plan.crates[dep].name);
    mlir::FailureOr<std::string> rootRs =
        emitrustcc::renderCrateRoot(module, type, depNames);
    if (mlir::failed(rootRs))
      return 1;
    llvm::SmallString<256> srcDir(outputPath);
    llvm::sys::path::append(srcDir, crate.name, "src");
    if (std::error_code ec = llvm::sys::fs::create_directories(srcDir)) {
      llvm::errs() << "error: cannot create directory '" << srcDir
                   << "': " << ec.message() << "\n";
      return 1;
    }
    llvm::SmallString<256> tomlPath(outputPath);
    llvm::sys::path::append(tomlPath, crate.name, "Cargo.toml");
    llvm::SmallString<256> rootPath(srcDir);
    llvm::sys::path::append(rootPath, emitrustcc::crateRootFileName(type));
    if (mlir::failed(writeFile(tomlPath, emitrustcc::renderMemberCargoToml(
                                             crate.name, type, depNames))))
      return 1;
    if (mlir::failed(writeFile(rootPath, *rootRs)))
      return 1;
  }

  if (buildFlag && mlir::failed(buildCrate(outputPath)))
    return 1;
  return 0;
}

//===----------------------------------------------------------------------===//
// FR-60 -- artifact queries: rejection report and ratchet manifest
//===----------------------------------------------------------------------===//

/// `--link --emit=rejection-report` and `--link --emit=ratchet`: pure
/// queries over the shard artifacts — no merge, no re-import, no C parsed
/// (deliberately, so both scale to a kernel link line and stay
/// deterministic functions of the artifacts alone; the ratchet's FR-59
/// partition facts are likewise planned over the RAW shards). Decodes each
/// shard's source path, ledger, and module-level definition count, then
/// hands everything to the pure renderers in RatchetReport.h. Under
/// `--ratchet-baseline` the comparison runs BEFORE the manifest is
/// written: any regression is a printed error and a nonzero exit.
///
/// \param inputs the link-line object/archive/shard paths, in link order.
/// \param context the context to parse the shards in.
/// \returns the process exit code.
static int emitLinkArtifactQuery(llvm::ArrayRef<std::string> inputs,
                                 mlir::MLIRContext &context) {
  llvm::SmallVector<LoadedShard> shards;
  if (!loadLinkShards(inputs, context, shards))
    return 1;

  llvm::SmallVector<emitrustcc::ShardFacts> facts;
  facts.reserve(shards.size());
  for (LoadedShard &shard : shards) {
    emitrustcc::ShardFacts shardFacts;
    std::optional<mlir::emitrust::ShardSource> source =
        mlir::emitrust::getShardSource(*shard.module);
    shardFacts.sourcePath = source ? source->path : shard.name;
    shardFacts.rejections =
        mlir::emitrust::getShardRejections(*shard.module);
    for (mlir::Operation &op : shard.module->getBody()->getOperations()) {
      if (op.hasAttr(mlir::emitrust::kExternDeclAttrName))
        continue;
      if (llvm::isa<mlir::SymbolOpInterface>(&op))
        ++shardFacts.definitionCount;
    }
    facts.push_back(std::move(shardFacts));
  }

  if (emitKind == EmitKind::RejectionReport)
    return mlir::failed(writeFile(outputPath,
                                  emitrustcc::renderRejectionReport(facts)))
               ? 1
               : 0;

  // --emit=ratchet.
  unsigned crates = 1;
  unsigned condensationWarnings = 0;
  if (partitionFlag) {
    llvm::SmallVector<emitrustcc::PartitionUnit> units(shards.size());
    for (auto [k, shard] : llvm::enumerate(shards)) {
      units[k].sourcePaths.push_back(facts[k].sourcePath);
      if (std::optional<llvm::StringRef> graph =
              mlir::emitrust::getShardItemGraph(*shard.module))
        units[k].graphTexts.push_back(graph->str());
      for (mlir::Operation &op : shard.module->getBody()->getOperations())
        if (auto impl = llvm::dyn_cast<mlir::emitrust::ImplOp>(&op))
          units[k].implTypes.push_back(impl.getStructName().str());
    }
    std::vector<std::pair<std::string, std::string>> overrides;
    if (!loadPartitionOverrides(overrides))
      return 1;
    emitrustcc::PartitionPlan plan = emitrustcc::planPartition(
        units, deducedCrateName(inputs), overrides);
    crates = static_cast<unsigned>(plan.crates.size());
    condensationWarnings = static_cast<unsigned>(plan.notes.size());
  }

  emitrustcc::RatchetManifest manifest =
      emitrustcc::buildRatchetManifest(facts, crates, condensationWarnings);

  if (!ratchetBaselinePath.empty()) {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> baselineFile =
        llvm::MemoryBuffer::getFile(ratchetBaselinePath);
    if (!baselineFile) {
      llvm::errs() << "error: cannot read --ratchet-baseline '"
                   << ratchetBaselinePath
                   << "': " << baselineFile.getError().message() << "\n";
      return 1;
    }
    emitrustcc::RatchetManifest baseline;
    std::string parseError;
    if (!emitrustcc::parseRatchetManifest((*baselineFile)->getBuffer(),
                                          baseline, parseError)) {
      llvm::errs() << "error: malformed --ratchet-baseline '"
                   << ratchetBaselinePath << "': " << parseError << "\n";
      return 1;
    }
    llvm::SmallVector<std::string> regressions =
        emitrustcc::compareRatchetManifests(baseline, manifest);
    for (const std::string &regression : regressions)
      llvm::errs() << "error: ratchet regression: " << regression << "\n";
    if (!regressions.empty())
      return 1;
    for (const std::string &improvement :
         emitrustcc::ratchetImprovements(baseline, manifest))
      llvm::errs() << "ratchet improvement: " << improvement << "\n";
  }

  return mlir::failed(writeFile(outputPath,
                                emitrustcc::renderRatchetManifest(manifest)))
             ? 1
             : 0;
}

/// Tool entry point: validates the option combination, imports the C inputs,
/// runs the lowering pipeline, and produces the selected output. Returns
/// nonzero on any error; diagnostics have already been printed.
int main(int argc, char **argv) {
  llvm::InitLLVM initLlvm(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv,
                                    "EmitRust C-to-Rust transpiler driver\n");

  // FR-53: enable the idiomatic Rust rename for every naming consumer (importer
  // and item graph) before any symbol is named. Set once here so the two paths
  // cannot disagree.
  mlir::emitrust::idiomaticRenameEnabled() = !preserveCNamesFlag;

  if (inputFilenames.empty() && compilationDatabasePath.empty()) {
    llvm::errs() << "error: at least one input file is required, or "
                    "--compdb <dir-or-file> to take the source list from a "
                    "compilation database\n";
    return 1;
  }
  if (buildFlag && emitKind != EmitKind::Crate) {
    llvm::errs() << "error: --build is only valid with --emit=crate\n";
    return 1;
  }
  // FR-58: --link consumes already-imported, already-converted shards, so
  // everything that configures an import or the pass pipeline is
  // meaningless with it and is rejected rather than silently ignored.
  // --emit=item-graph is one exception (FR-57d): the shards CARRY their
  // item-graph text, so dumping it per shard needs no import; FR-60's
  // rejection-report and ratchet are the other two, for the same reason.
  if (linkFlag && emitKind != EmitKind::Rust && emitKind != EmitKind::Crate &&
      emitKind != EmitKind::ItemGraph &&
      emitKind != EmitKind::RejectionReport && emitKind != EmitKind::Ratchet) {
    llvm::errs() << "error: --link merges already-converted shards, so only "
                    "--emit=rust, --emit=crate, --emit=item-graph, "
                    "--emit=rejection-report and --emit=ratchet apply\n";
    return 1;
  }
  // FR-59: partitioning is a link-time, crate-emitting operation only —
  // plus FR-60's ratchet, whose manifest records the partition facts.
  if (partitionFlag &&
      (!linkFlag ||
       (emitKind != EmitKind::Crate && emitKind != EmitKind::Ratchet))) {
    llvm::errs() << "error: --partition requires --link and --emit=crate\n";
    return 1;
  }
  if (!partitionMapPath.empty() && !partitionFlag) {
    llvm::errs() << "error: --partition-map requires --partition\n";
    return 1;
  }
  // FR-60: both artifact queries read the shard ledgers off the link line.
  if (emitKind == EmitKind::RejectionReport && !linkFlag) {
    llvm::errs() << "error: --emit=rejection-report requires --link\n";
    return 1;
  }
  if (emitKind == EmitKind::Ratchet && !linkFlag) {
    llvm::errs() << "error: --emit=ratchet requires --link\n";
    return 1;
  }
  if (!ratchetBaselinePath.empty() && emitKind != EmitKind::Ratchet) {
    llvm::errs() << "error: --ratchet-baseline requires --emit=ratchet\n";
    return 1;
  }
  if (linkFlag &&
      (!compilationDatabasePath.empty() || recoverFlag || incrementalFlag ||
       searchFlag || deferExternalsFlag || checkRangeRefinement)) {
    llvm::errs() << "error: --link takes object/shard files, not C sources; "
                    "--compdb, --recover, --incremental, --search, "
                    "--defer-externals and --check-range-refinement do not "
                    "apply\n";
    return 1;
  }
  if (incrementalFlag && emitKind != EmitKind::Crate) {
    llvm::errs() << "error: --incremental is only valid with --emit=crate\n";
    return 1;
  }
  // FR-51: --crate-type selects a crate SHAPE, so it is meaningful exactly
  // where a crate (or its root source) is produced.
  if (crateTypeOpt != emitrustcc::CrateTypeRequest::Auto &&
      emitKind != EmitKind::Crate && emitKind != EmitKind::Rust) {
    llvm::errs() << "error: --crate-type is only valid with --emit=crate or "
                    "--emit=rust\n";
    return 1;
  }
  if (emitKind == EmitKind::Crate && outputPath == "-") {
    llvm::errs() << "error: --emit=crate requires -o <crate directory>\n";
    return 1;
  }
  // FR-43: the search picks the ITEM SET of an incremental crate, so it is
  // only meaningful where there is an item set to pick — and `--emit=search`
  // is the search with the crate left off, so it implies the flag rather than
  // conflicting with it.
  bool runSearch = searchFlag || emitKind == EmitKind::Search;
  if (searchFlag && emitKind != EmitKind::Crate &&
      emitKind != EmitKind::Search) {
    llvm::errs() << "error: --search is only valid with --emit=crate "
                    "--incremental or --emit=search\n";
    return 1;
  }
  if (searchFlag && emitKind == EmitKind::Crate && !incrementalFlag) {
    llvm::errs() << "error: --search requires --incremental: the search "
                    "chooses which items the crate contains, which only an "
                    "incremental crate can omit\n";
    return 1;
  }

  std::vector<std::string> inputs(inputFilenames.begin(),
                                  inputFilenames.end());
  std::vector<std::string> extra = collectExtraClangArgs();

  // FR-57d: under --link the item graph comes from the shards' stored
  // metadata, per shard in link-line order — no C is parsed at all.
  if (linkFlag && emitKind == EmitKind::ItemGraph)
    return emitLinkShardItemGraphs(inputs, outputPath);

  // The item graph and the coloring are pure-AST analyses: no MLIR context,
  // no import, no pipeline. Handled here, before any of that machinery is set
  // up, so that both stay obtainable for a project the importer would reject.
  if (isProjectAnalysis(emitKind)) {
    // --compdb applies here exactly as it does to an import (FR-45): the
    // analysis must see the same project, with the same per-file flags, or it
    // would describe a project the importer never compiles.
    std::string databaseError;
    std::string text;
    bool ok = false;
    if (emitKind == EmitKind::ItemGraph) {
      mlir::FailureOr<mlir::emitrust::ItemGraph> graph =
          mlir::emitrust::buildItemGraph(inputs, extra, compilationDatabasePath,
                                         databaseError);
      if ((ok = mlir::succeeded(graph)))
        text = graph->print();
    } else {
      mlir::FailureOr<mlir::emitrust::ItemColoring> coloring =
          mlir::emitrust::colorItems(inputs, extra, compilationDatabasePath,
                                     databaseError);
      if ((ok = mlir::succeeded(coloring)))
        text = coloring->print();
    }
    if (!ok) {
      // A database load failure has its own reason; otherwise clang has
      // already printed the parse diagnostics to stderr and neither analysis
      // raises anything else.
      if (!databaseError.empty())
        llvm::errs() << compilationDatabasePath
                     << ":1:1: error: cannot load compilation database: "
                     << databaseError << "\n";
      else
        llvm::errs() << "error: failed to parse one or more inputs\n";
      return 1;
    }
    return mlir::failed(writeFile(outputPath, text)) ? 1 : 0;
  }

  // FR-43: the frontier search runs BEFORE the compile it configures. Its
  // output is one thing — the set of items to exclude — which then flows into
  // the ordinary recovering import below, so everything after this block is
  // the same code path a plain `--incremental` run takes. The search's own
  // project analyses are kept: they were parsed once here, and reusing them
  // for FR-44's denominator saves the report an otherwise identical parse.
  std::optional<mlir::emitrust::SearchInputs> searchInputs;
  std::set<std::string> searchExcluded;
  if (runSearch) {
    std::string databaseError;
    mlir::FailureOr<mlir::emitrust::SearchInputs> built =
        mlir::emitrust::buildSearchInputs(inputs, extra,
                                          compilationDatabasePath,
                                          databaseError);
    if (mlir::failed(built)) {
      if (!databaseError.empty())
        llvm::errs() << compilationDatabasePath
                     << ":1:1: error: cannot load compilation database: "
                     << databaseError << "\n";
      else
        llvm::errs() << "error: failed to parse one or more inputs\n";
      return 1;
    }
    searchInputs = std::move(*built);
    std::string crateName = deducedCrateName(inputs);
    mlir::emitrust::SearchOptions searchOptions;
    searchOptions.maxNodes = maxSearchNodes;
    mlir::emitrust::SearchResult search = mlir::emitrust::frontierSearch(
        searchInputs->graph, searchInputs->admissibility, searchOptions,
        [&](const mlir::emitrust::SearchState &state) {
          return probeSearchState(*searchInputs, inputs, extra, crateName,
                                  state);
        });
    searchExcluded =
        mlir::emitrust::excludedItemsFor(searchInputs->graph, search.best);
    // FR-50, enforced in the shipped build. `frontierSearch` asserts the same
    // postcondition, but an assertion is compiled out of a release build and
    // this is exactly the guarantee a release user is relying on: `--search`
    // must never hand back less than plain `--incremental` would have. The
    // property is structural — `best` is a maximum over probed states and the
    // admit-everything state is always one of them — so reaching this branch
    // means the probe or the score is not a function of the state. Say so
    // loudly, then fall back to the unrestricted import, which is the answer
    // the user would have got without the flag.
    if (!search.atLeastBaseline()) {
      llvm::errs()
          << "error: internal: the FR-43 search scored below the "
             "unrestricted import (ported "
          << search.bestScore.ported << " vs " << search.baselineScore.ported
          << "); falling back to --incremental without --search. Please "
             "report this with --emit=search output.\n";
      searchExcluded.clear();
    }
    if (emitKind == EmitKind::Search)
      return mlir::failed(writeFile(outputPath, search.trace)) ? 1 : 0;
    if (!searchTracePath.empty() &&
        mlir::failed(writeFile(searchTracePath == "-" ? llvm::StringRef("/dev/stderr")
                                                      : llvm::StringRef(searchTracePath),
                               search.trace)))
      return 1;
  }

  mlir::MLIRContext context;
  context.getDiagEngine().registerHandler(
      [](mlir::Diagnostic &diag) { printDiagnostic(diag); });
  // importC loads emitrust/func/arith/memref/cf; the passes declare their
  // own dependent dialects, but load the lifting targets explicitly so the
  // context never depends on pass-internal registration details.
  context.loadDialect<mlir::scf::SCFDialect, mlir::ub::UBDialect>();

  // FR-42: the ledger outlives the import so the summary can be printed
  // after the module has been produced (and, on a recovered compile, after
  // the pipeline has confirmed the surviving subset is still lowerable).
  mlir::emitrust::RejectionLedger ledger;
  mlir::OwningOpRef<mlir::ModuleOp> module;
  if (linkFlag) {
    // FR-60: pure artifact queries, dispatched before any merge.
    if (emitKind == EmitKind::RejectionReport ||
        emitKind == EmitKind::Ratchet)
      return emitLinkArtifactQuery(inputs, context);
    // FR-59: partitioning is a whole path of its own — it needs the
    // per-shard facts the ordinary merge deliberately strips.
    if (partitionFlag)
      return emitPartitionedWorkspace(inputs, context);
    // FR-58: the positional inputs are objects/shards, every one of them
    // already imported AND already lowered by the FR-56 shim, so the merge
    // replaces both the import and the pass pipeline below and the merged
    // module goes straight to the emission switch.
    module = loadAndMergeShards(inputs, context);
    if (!module)
      return 1;
  } else {
    mlir::emitrust::ImportOptions importOptions;
    // --incremental IMPLIES --recover: a partial crate is the whole point,
    // and an incremental run that hard-failed at the first unsupported item
    // could not report a per-item status for anything after it.
    importOptions.recover = recoverFlag || incrementalFlag;
    importOptions.ledger = &ledger;
    // --recover and --compdb are the pair a real C++ project needs together:
    // the database to be parsed the way its build system parses it, recovery
    // to yield anything at all.
    importOptions.compilationDatabasePath = compilationDatabasePath;
    // FR-43: the search's answer enters the compile here and nowhere else.
    // An excluded item takes FR-42's recovery path (stub if its signature
    // maps, drop otherwise), so the crate emitted below is a crate the
    // recovering importer already knew how to build — the search chose
    // WHICH one.
    importOptions.excludedItems = searchExcluded;
    // FR-52: an unresolved external is an ERROR for a binary crate and a
    // REQUIREMENT for a library one; see `externalRequirementsPolicy`.
    importOptions.externalRequirements = externalRequirementsPolicy();
    // FR-57a: per-TU shim-path import mode; the emitted module carries
    // declaration stubs the FR-58 link step must resolve, and the Rust
    // emitter refuses a module still carrying one.
    importOptions.deferExternals = deferExternalsFlag;
    module = mlir::emitrust::importCProject(inputs, extra, importOptions,
                                            context);
    if (!module)
      return 1;
    // Printed before any output is written so it is visible even when a
    // later stage fails; a no-op when nothing was recovered.
    ledger.printSummary(llvm::errs());

    if (emitKind == EmitKind::Import)
      return mlir::failed(writeModule(*module, outputPath)) ? 1 : 0;

    if (mlir::failed(runPipeline(*module)))
      return 1;
  }

  switch (emitKind.getValue()) {
  case EmitKind::ItemGraph:
  case EmitKind::Coloring:
    llvm_unreachable("handled before the import");
  case EmitKind::RejectionReport:
  case EmitKind::Ratchet:
    llvm_unreachable("handled inside the --link dispatch");
  case EmitKind::Search:
    llvm_unreachable("handled before the import");
  case EmitKind::Import:
    llvm_unreachable("handled before the pipeline");
  case EmitKind::MLIR:
    return mlir::failed(writeModule(*module, outputPath)) ? 1 : 0;
  case EmitKind::Rust: {
    // FR-51: `--emit=rust` is the crate ROOT, whichever shape the crate would
    // have. That invariant used to hold only for an input with a `main` (the
    // no-`main` case printed a bare translation, because there was no library
    // crate for it to be the root of); now it is total.
    emitrustcc::CrateType type =
        emitrustcc::selectCrateType(crateTypeOpt, *module);
    if (mlir::failed(diagnoseCrateType(*module, type)))
      return 1;
    mlir::FailureOr<std::string> source =
        emitrustcc::renderCrateRoot(*module, type);
    if (mlir::failed(source))
      return 1;
    return mlir::failed(writeFile(outputPath, *source)) ? 1 : 0;
  }
  case EmitKind::Crate: {
    emitrustcc::CrateType type =
        emitrustcc::selectCrateType(crateTypeOpt, *module);
    if (mlir::failed(diagnoseCrateType(*module, type)))
      return 1;
    std::string crateName = deducedCrateName(inputs);
    // FR-44: the emitted symbol table is read BEFORE the crate is written,
    // because it is evidence about the very module being rendered.
    llvm::StringSet<> emittedSymbols;
    if (incrementalFlag)
      emittedSymbols = emitrustcc::collectEmittedSymbols(*module);
    if (mlir::failed(emitCrate(*module, outputPath, crateName, type)))
      return 1;
    if (incrementalFlag) {
      // The DENOMINATOR comes from the FR-40 item graph, which is a second,
      // purely analytical parse of the same project: it inventories every
      // item the project HAS, including the ones the importer rejected, so
      // "2 of 8 ported" is a real fraction rather than a count over whatever
      // survived. It re-parses the inputs, which is why it is computed only
      // under --incremental — and not at all under --search, which already
      // parsed the project once to build the very same graph.
      //
      // FR-49 needs the FR-41 COLORING of that same graph as well, to credit
      // each rejection to its root cause. `buildSearchInputs` computes graph,
      // admissibility and coloring from ONE parse — the same parse the graph
      // alone would have cost — so root attribution is free here, and the
      // --search path below already has all three for the same reason.
      std::string databaseError;
      mlir::FailureOr<mlir::emitrust::SearchInputs> analyses = mlir::failure();
      if (!searchInputs) {
        analyses = mlir::emitrust::buildSearchInputs(
            inputs, extra, compilationDatabasePath, databaseError);
        if (mlir::failed(analyses))
          llvm::errs() << "warning: cannot build the project item graph"
                       << (databaseError.empty() ? "" : ": ")
                       << databaseError
                       << "; the progress report has no denominator "
                          "(denominator_source: ledger-only)\n";
      }
      const mlir::emitrust::SearchInputs *joined =
          searchInputs                 ? &*searchInputs
          : mlir::succeeded(analyses)  ? &*analyses
                                       : nullptr;
      emitrustcc::ProgressReport report = emitrustcc::buildProgressReport(
          crateName, joined ? &joined->graph : nullptr,
          joined ? &joined->coloring : nullptr, ledger.getItems(),
          emittedSymbols);
      if (mlir::failed(writeProgressArtifacts(outputPath, report)))
        return 1;
    }
    if (buildFlag && mlir::failed(buildCrate(outputPath)))
      return 1;
    return 0;
  }
  }
  llvm_unreachable("covered switch");
}

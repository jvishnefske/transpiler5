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
#include "ProgressReport.h"

#include "EmitRust/CSymbolNaming.h"
#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/Conversion/ConvertToEmitRust.h"
#include "EmitRust/Conversion/LowerContainers.h"
#include "EmitRust/Conversion/LowerExternalRequirements.h"
#include "EmitRust/Conversion/RangeRefinementCheck.h"
#include "EmitRust/ImportC.h"
#include "EmitRust/Project/FrontierSearch.h"
#include "EmitRust/Project/ItemColoring.h"
#include "EmitRust/Project/ItemGraph.h"

#include "mlir/Conversion/ControlFlowToSCF/ControlFlowToSCF.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/FileUtilities.h"
#include "mlir/Transforms/Passes.h"

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
enum class EmitKind { ItemGraph, Coloring, Import, MLIR, Rust, Crate, Search };

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
                   "project need not define main")),
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

/// Loads, extracts, parses, and merges the link-line `inputs` into one
/// whole-program module (FR-58 slice 1, `--link`). The functional core is
/// `emitrustcc::findShardPayload` (payload location inside one input) and
/// `emitrustcc::mergeLinkShards` (the spike-proven merge); this function
/// owns the file reads and the parse. Each input is an object file whose
/// `.emitrust` section carries the shard, an object falling back to its
/// `<object>.emitrust.mlirbc` sidecar, or a shard file named directly —
/// `parseSourceFile` handles MLIR bytecode and text alike. Every failure
/// path has printed a diagnostic by the time this returns null.
///
/// \param inputs the link-line object/shard paths, in link order.
/// \param context the context to parse and merge in; the EmitRust dialect
///        is loaded here because no import will do it.
/// \returns the merged, verified whole-program module, or null.
static mlir::OwningOpRef<mlir::ModuleOp>
loadAndMergeShards(llvm::ArrayRef<std::string> inputs,
                   mlir::MLIRContext &context) {
  context.loadDialect<mlir::emitrust::EmitRustDialect>();
  llvm::SmallVector<mlir::OwningOpRef<mlir::ModuleOp>> shards;
  for (const std::string &path : inputs) {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> file =
        llvm::MemoryBuffer::getFile(path);
    if (!file) {
      llvm::errs() << "error: cannot read '" << path
                   << "': " << file.getError().message() << "\n";
      return nullptr;
    }
    std::string sectionError;
    mlir::FailureOr<std::optional<llvm::StringRef>> payload =
        emitrustcc::findShardPayload(**file, sectionError);
    if (mlir::failed(payload)) {
      llvm::errs() << "error: cannot read the .emitrust section of '" << path
                   << "': " << sectionError << "\n";
      return nullptr;
    }
    std::unique_ptr<llvm::MemoryBuffer> moduleBuffer;
    if (*payload) {
      // Copied out because the section contents point into `file`, which
      // dies with this iteration, while parser diagnostics and the parsed
      // module's string attrs must not.
      moduleBuffer = llvm::MemoryBuffer::getMemBufferCopy(**payload, path);
    } else {
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
        return nullptr;
      }
      moduleBuffer = std::move(*side);
    }
    llvm::SourceMgr sourceMgr;
    sourceMgr.AddNewSourceBuffer(std::move(moduleBuffer), llvm::SMLoc());
    mlir::ParserConfig parserConfig(&context);
    mlir::OwningOpRef<mlir::ModuleOp> shard =
        mlir::parseSourceFile<mlir::ModuleOp>(sourceMgr, parserConfig);
    if (!shard)
      return nullptr;
    shards.push_back(std::move(shard));
  }
  mlir::FailureOr<mlir::OwningOpRef<mlir::ModuleOp>> merged =
      emitrustcc::mergeLinkShards(shards);
  if (mlir::failed(merged))
    return nullptr;
  return std::move(*merged);
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
  if (linkFlag && emitKind != EmitKind::Rust && emitKind != EmitKind::Crate) {
    llvm::errs() << "error: --link merges already-converted shards, so only "
                    "--emit=rust and --emit=crate apply\n";
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

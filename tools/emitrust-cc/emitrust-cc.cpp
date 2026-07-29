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
///   --emit=import  the raw imported MLIR module, before any pass;
///   --emit=mlir    the MLIR module after the full pass pipeline;
///   --emit=rust    Rust source text (identical to the crate's src/main.rs
///                  when the input defines main, else the bare translation);
///   --emit=crate   a complete cargo crate directory (the default), with
///                  --build optionally invoking `cargo build --release
///                  --offline` on the result, and --incremental (FR-44)
///                  additionally recovering from unsupported items and
///                  writing PORTING.md / emitrust-progress.json beside the
///                  crate.
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
#include "ProgressReport.h"

#include "EmitRust/Conversion/ConvertToEmitRust.h"
#include "EmitRust/Conversion/RangeRefinementCheck.h"
#include "EmitRust/ImportC.h"
#include "EmitRust/Project/ItemGraph.h"

#include "mlir/Conversion/ControlFlowToSCF/ControlFlowToSCF.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
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
#include "llvm/Support/Path.h"
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
/// `ItemGraph` is the odd one out: every other kind is a stage of the
/// import-and-lower pipeline, while the item graph is a parallel, pure-AST
/// analysis that never builds a module. It is handled before the import for
/// exactly that reason — the graph is meant to be obtainable for a project
/// the importer cannot yet translate.
enum class EmitKind { ItemGraph, Import, MLIR, Rust, Crate };

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
        clEnumValN(EmitKind::Import, "import",
                   "Raw imported MLIR module, before any pass (debugging)"),
        clEnumValN(EmitKind::MLIR, "mlir",
                   "MLIR module after the full pass pipeline, i.e. the "
                   "emitter's input (debugging)"),
        clEnumValN(EmitKind::Rust, "rust",
                   "Rust source text; when the input defines main this is "
                   "byte-identical to the crate's src/main.rs (allow-header, "
                   "translation, fn main wrapper), otherwise it is the bare "
                   "translation"),
        clEnumValN(EmitKind::Crate, "crate",
                   "Complete cargo crate directory; -o names the crate "
                   "directory and the input must define main")),
    llvm::cl::init(EmitKind::Crate));

static llvm::cl::opt<std::string>
    outputPath("o",
               llvm::cl::desc("Output file for --emit=import/mlir/rust "
                              "('-' means stdout); output crate directory "
                              "for --emit=crate (required)"),
               llvm::cl::value_desc("path"), llvm::cl::init("-"));

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
  pm.addPass(mlir::createMem2Reg());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::createLiftControlFlowToSCFPass());
  pm.addPass(mlir::createCanonicalizerPass());
  if (checkRangeRefinement)
    pm.addPass(mlir::emitrust::createEmitRustRangeRefinementCheck());
  pm.addPass(mlir::emitrust::createConvertToEmitRust());
  return pm.run(module);
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
/// `Cargo.toml` and `src/main.rs` first (pure, no partial output on a
/// translation failure), then creates the directories and writes both files.
///
/// \param module the fully converted module; must define `c_main`.
/// \param outDir the crate directory to create and populate.
/// \param crateName the sanitized cargo package name.
/// \returns success if the whole crate was written.
static mlir::LogicalResult emitCrate(mlir::ModuleOp module,
                                     llvm::StringRef outDir,
                                     llvm::StringRef crateName) {
  mlir::FailureOr<std::string> mainRs = emitrustcc::renderRustSource(module);
  if (mlir::failed(mainRs))
    return mlir::failure();
  std::string cargoToml = emitrustcc::renderCargoToml(crateName);

  llvm::SmallString<256> srcDir(outDir);
  llvm::sys::path::append(srcDir, "src");
  if (std::error_code ec = llvm::sys::fs::create_directories(srcDir)) {
    llvm::errs() << "error: cannot create directory '" << srcDir
                 << "': " << ec.message() << "\n";
    return mlir::failure();
  }

  llvm::SmallString<256> tomlPath(outDir);
  llvm::sys::path::append(tomlPath, "Cargo.toml");
  llvm::SmallString<256> mainPath(srcDir);
  llvm::sys::path::append(mainPath, "main.rs");
  if (mlir::failed(writeFile(tomlPath, cargoToml)))
    return mlir::failure();
  return writeFile(mainPath, *mainRs);
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
  if (incrementalFlag && emitKind != EmitKind::Crate) {
    llvm::errs() << "error: --incremental is only valid with --emit=crate\n";
    return 1;
  }
  if (emitKind == EmitKind::Crate && outputPath == "-") {
    llvm::errs() << "error: --emit=crate requires -o <crate directory>\n";
    return 1;
  }

  std::vector<std::string> inputs(inputFilenames.begin(),
                                  inputFilenames.end());
  std::vector<std::string> extra = collectExtraClangArgs();

  // The item graph is a pure-AST analysis: no MLIR context, no import, no
  // pipeline. Handled here, before any of that machinery is set up, so the
  // graph of a project the importer would reject is still obtainable.
  if (emitKind == EmitKind::ItemGraph) {
    // --compdb applies here exactly as it does to an import (FR-45): the
    // graph must see the same project, with the same per-file flags, or it
    // would describe a project the importer never compiles.
    std::string databaseError;
    mlir::FailureOr<mlir::emitrust::ItemGraph> graph =
        mlir::emitrust::buildItemGraph(inputs, extra, compilationDatabasePath,
                                       databaseError);
    if (mlir::failed(graph)) {
      // A database load failure has its own reason; otherwise clang has
      // already printed the parse diagnostics to stderr and the graph itself
      // raises nothing else.
      if (!databaseError.empty())
        llvm::errs() << compilationDatabasePath
                     << ":1:1: error: cannot load compilation database: "
                     << databaseError << "\n";
      else
        llvm::errs() << "error: failed to parse one or more inputs\n";
      return 1;
    }
    return mlir::failed(writeFile(outputPath, graph->print())) ? 1 : 0;
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
  mlir::emitrust::ImportOptions importOptions;
  // --incremental IMPLIES --recover: a partial crate is the whole point, and
  // an incremental run that hard-failed at the first unsupported item could
  // not report a per-item status for anything after it.
  importOptions.recover = recoverFlag || incrementalFlag;
  importOptions.ledger = &ledger;
  // --recover and --compdb are the pair a real C++ project needs together:
  // the database to be parsed the way its build system parses it, recovery
  // to yield anything at all.
  importOptions.compilationDatabasePath = compilationDatabasePath;
  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::emitrust::importCProject(inputs, extra, importOptions, context);
  if (!module)
    return 1;
  // Printed before any output is written so it is visible even when a later
  // stage fails; a no-op when nothing was recovered.
  ledger.printSummary(llvm::errs());

  if (emitKind == EmitKind::Import)
    return mlir::failed(writeModule(*module, outputPath)) ? 1 : 0;

  if (mlir::failed(runPipeline(*module)))
    return 1;

  switch (emitKind.getValue()) {
  case EmitKind::ItemGraph:
    llvm_unreachable("handled before the import");
  case EmitKind::Import:
    llvm_unreachable("handled before the pipeline");
  case EmitKind::MLIR:
    return mlir::failed(writeModule(*module, outputPath)) ? 1 : 0;
  case EmitKind::Rust: {
    mlir::FailureOr<std::string> source =
        emitrustcc::renderRustSource(*module);
    if (mlir::failed(source))
      return 1;
    return mlir::failed(writeFile(outputPath, *source)) ? 1 : 0;
  }
  case EmitKind::Crate: {
    if (!emitrustcc::hasCMain(*module)) {
      module->emitError()
          << "cannot emit a crate: the input does not define a 'main' "
             "function (imported as 'c_main')";
      return 1;
    }
    // The crate name comes from --crate-name when given; otherwise, for a
    // single input its stem (historical behavior), and for several inputs the
    // -o crate-directory stem (the input stems are ambiguous). A --compdb
    // run with no positional inputs takes the same crate-directory stem:
    // the database's file list is a project, not one nameable input.
    llvm::StringRef crateStem = !crateNameOpt.empty()
                                    ? llvm::StringRef(crateNameOpt)
                                : inputs.size() == 1
                                    ? llvm::sys::path::stem(inputs.front())
                                    : llvm::sys::path::stem(outputPath);
    std::string crateName = emitrustcc::sanitizeCrateName(crateStem);
    // FR-44: the emitted symbol table is read BEFORE the crate is written,
    // because it is evidence about the very module being rendered.
    llvm::StringSet<> emittedSymbols;
    if (incrementalFlag)
      emittedSymbols = emitrustcc::collectEmittedSymbols(*module);
    if (mlir::failed(emitCrate(*module, outputPath, crateName)))
      return 1;
    if (incrementalFlag) {
      // The DENOMINATOR comes from the FR-40 item graph, which is a second,
      // purely analytical parse of the same project: it inventories every
      // item the project HAS, including the ones the importer rejected, so
      // "2 of 8 ported" is a real fraction rather than a count over whatever
      // survived. It re-parses the inputs, which is why it is computed only
      // under --incremental.
      std::string databaseError;
      mlir::FailureOr<mlir::emitrust::ItemGraph> graph =
          mlir::emitrust::buildItemGraph(inputs, extra, compilationDatabasePath,
                                         databaseError);
      if (mlir::failed(graph))
        llvm::errs() << "warning: cannot build the project item graph"
                     << (databaseError.empty() ? "" : ": ")
                     << databaseError
                     << "; the progress report has no denominator "
                        "(denominator_source: ledger-only)\n";
      emitrustcc::ProgressReport report = emitrustcc::buildProgressReport(
          crateName, mlir::succeeded(graph) ? &*graph : nullptr,
          ledger.getItems(), emittedSymbols);
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

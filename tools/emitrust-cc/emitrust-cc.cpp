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
///   --emit=import  the raw imported MLIR module, before any pass;
///   --emit=mlir    the MLIR module after the full pass pipeline;
///   --emit=rust    Rust source text (identical to the crate's src/main.rs
///                  when the input defines main, else the bare translation);
///   --emit=crate   a complete cargo crate directory (the default), with
///                  --build optionally invoking `cargo build --release
///                  --offline` on the result.
/// All content rendering is delegated to the pure functions in
/// CrateEmitter.h; this file owns diagnostics, filesystem writes, and
/// process invocation. Any failure produces a located diagnostic and a
/// nonzero exit code; no partial output is ever kept.
//
//===----------------------------------------------------------------------===//

#include "CrateEmitter.h"

#include "EmitRust/Conversion/ConvertToEmitRust.h"
#include "EmitRust/Conversion/RangeRefinementCheck.h"
#include "EmitRust/ImportC.h"

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
enum class EmitKind { Import, MLIR, Rust, Crate };

} // namespace

static llvm::cl::list<std::string>
    inputFilenames(llvm::cl::Positional, llvm::cl::desc("<input C files>"),
                   llvm::cl::OneOrMore);

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

  if (buildFlag && emitKind != EmitKind::Crate) {
    llvm::errs() << "error: --build is only valid with --emit=crate\n";
    return 1;
  }
  if (emitKind == EmitKind::Crate && outputPath == "-") {
    llvm::errs() << "error: --emit=crate requires -o <crate directory>\n";
    return 1;
  }

  mlir::MLIRContext context;
  context.getDiagEngine().registerHandler(
      [](mlir::Diagnostic &diag) { printDiagnostic(diag); });
  // importC loads emitrust/func/arith/memref/cf; the passes declare their
  // own dependent dialects, but load the lifting targets explicitly so the
  // context never depends on pass-internal registration details.
  context.loadDialect<mlir::scf::SCFDialect, mlir::ub::UBDialect>();

  std::vector<std::string> inputs(inputFilenames.begin(),
                                  inputFilenames.end());
  std::vector<std::string> extra = collectExtraClangArgs();
  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::emitrust::importCProject(inputs, extra, context);
  if (!module)
    return 1;

  if (emitKind == EmitKind::Import)
    return mlir::failed(writeModule(*module, outputPath)) ? 1 : 0;

  if (mlir::failed(runPipeline(*module)))
    return 1;

  switch (emitKind.getValue()) {
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
    // -o crate-directory stem (the input stems are ambiguous).
    llvm::StringRef crateStem = !crateNameOpt.empty()
                                    ? llvm::StringRef(crateNameOpt)
                                : inputs.size() == 1
                                    ? llvm::sys::path::stem(inputs.front())
                                    : llvm::sys::path::stem(outputPath);
    std::string crateName = emitrustcc::sanitizeCrateName(crateStem);
    if (mlir::failed(emitCrate(*module, outputPath, crateName)))
      return 1;
    if (buildFlag && mlir::failed(buildCrate(outputPath)))
      return 1;
    return 0;
  }
  }
  llvm_unreachable("covered switch");
}

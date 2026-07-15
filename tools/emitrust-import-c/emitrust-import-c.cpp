//===- emitrust-import-c.cpp - C-to-EmitRust importer driver -------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file is the imperative shell of the C importer tool: it parses the
/// command line, installs a file:line:col diagnostic printer, invokes the
/// importC library entry point, and writes the resulting MLIR module to the
/// selected output. Any diagnostic results in a nonzero exit code.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/ImportC.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Support/FileUtilities.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

static llvm::cl::list<std::string>
    inputFilenames(llvm::cl::Positional, llvm::cl::desc("<input C files>"),
                   llvm::cl::OneOrMore);

static llvm::cl::opt<std::string>
    outputFilename("o", llvm::cl::desc("Output filename"),
                   llvm::cl::value_desc("filename"), llvm::cl::init("-"));

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

/// Tool entry point: imports the given C file and prints the MLIR module to
/// stdout or to the file given with -o. Returns nonzero on any error.
int main(int argc, char **argv) {
  llvm::InitLLVM initLlvm(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv, "EmitRust C importer\n");

  mlir::MLIRContext context;
  context.getDiagEngine().registerHandler(
      [](mlir::Diagnostic &diag) { printDiagnostic(diag); });

  std::vector<std::string> extra = collectExtraClangArgs();
  std::vector<std::string> inputs(inputFilenames.begin(),
                                  inputFilenames.end());
  // A single input keeps the historical single-TU behavior (bare names); two
  // or more inputs are merged as a project with cross-TU linkage.
  mlir::OwningOpRef<mlir::ModuleOp> module =
      inputs.size() == 1
          ? mlir::emitrust::importC(inputs.front(), extra, context)
          : mlir::emitrust::importCProject(inputs, extra, context);
  if (!module)
    return 1;

  std::string errorMessage;
  std::unique_ptr<llvm::ToolOutputFile> output =
      mlir::openOutputFile(outputFilename, &errorMessage);
  if (!output) {
    llvm::errs() << errorMessage << "\n";
    return 1;
  }
  module->print(output->os());
  output->os() << "\n";
  output->keep();
  return 0;
}

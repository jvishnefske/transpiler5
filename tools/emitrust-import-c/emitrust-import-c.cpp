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

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>

static llvm::cl::opt<std::string>
    inputFilename(llvm::cl::Positional, llvm::cl::desc("<input C file>"),
                  llvm::cl::Required);

static llvm::cl::opt<std::string>
    outputFilename("o", llvm::cl::desc("Output filename"),
                   llvm::cl::value_desc("filename"), llvm::cl::init("-"));

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

  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::emitrust::importC(inputFilename, context);
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

//===- emitrust-translate.cpp - EmitRust translation driver --------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file is the imperative shell of the EmitRust translation tool: it
/// registers the `--mlir-to-rust` translation and hands all CLI, parsing,
/// and I/O concerns to the shared mlir-translate driver.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Target/TranslateToRust.h"

#include "mlir/Tools/mlir-translate/MlirTranslateMain.h"

/// Tool entry point: registers the Rust translation and delegates to the
/// mlir-translate main driver.
int main(int argc, char **argv) {
  mlir::emitrust::registerToRustTranslation();
  return mlir::failed(
      mlir::mlirTranslateMain(argc, argv, "EmitRust translation tool"));
}

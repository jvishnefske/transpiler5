//===- emitrust-opt.cpp - EmitRust optimizer driver -----------*- C++ -*-===//
//
// Standard mlir-opt-style driver with the EmitRust dialect and the
// EmitRust conversion passes registered, used by the lit regression suite
// for parse/print round-trip, verifier, and conversion tests.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/Passes.h"
#include "EmitRust/EmitRustDialect.h"

#include "mlir/IR/DialectRegistry.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllExtensions.h"
#include "mlir/InitAllPasses.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

int main(int argc, char **argv) {
  mlir::registerAllPasses();
  mlir::emitrust::registerEmitRustConversionPasses();

  mlir::DialectRegistry registry;
  mlir::registerAllDialects(registry);
  mlir::registerAllExtensions(registry);
  registry.insert<mlir::emitrust::EmitRustDialect>();

  return mlir::asMainReturnCode(
      mlir::MlirOptMain(argc, argv, "EmitRust optimizer driver\n", registry));
}

//===- TranslateRegistration.cpp - Register translation to Rust ----------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file registers the EmitRust-to-Rust translation with the global
/// translation registry under the `--mlir-to-rust` flag, mirroring how
/// upstream EmitC registers `registerToCppTranslation`.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/Target/TranslateToRust.h"

#include "mlir/IR/BuiltinDialect.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/Operation.h"
#include "mlir/Tools/mlir-translate/Translation.h"

namespace mlir {
namespace emitrust {

//===----------------------------------------------------------------------===//
// EmitRust dialect to Rust translation registration
//===----------------------------------------------------------------------===//

void registerToRustTranslation() {
  TranslateFromMLIRRegistration reg(
      "mlir-to-rust", "translate the EmitRust dialect to Rust source code",
      [](Operation *op, raw_ostream &output) {
        return translateToRust(op, output);
      },
      [](DialectRegistry &registry) {
        // clang-format off
        registry.insert<emitrust::EmitRustDialect,
                        BuiltinDialect>();
        // clang-format on
      });
}

} // namespace emitrust
} // namespace mlir

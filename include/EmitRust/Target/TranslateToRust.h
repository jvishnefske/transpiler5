//===- TranslateToRust.h - Helpers to create Rust emitter ------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file declares the public entry points of the EmitRust-to-Rust
/// translation: a pure translation function from an MLIR operation tree to
/// Rust source text, and the registration hook that exposes the translation
/// as `--mlir-to-rust` in mlir-translate-style tools. It mirrors the shape of
/// upstream EmitC's `mlir/Target/Cpp/CppEmitter.h`.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_TARGET_TRANSLATETORUST_H
#define EMITRUST_TARGET_TRANSLATETORUST_H

#include "mlir/Support/LLVM.h"

namespace mlir {
class Operation;

namespace emitrust {

/// Translates the given operation to Rust source code written to `os`.
///
/// The operation is expected to be a `builtin.module` whose children are
/// EmitRust `use`, `verbatim`, and `func` operations, or one of those
/// operations directly. Every construct that cannot be represented in Rust
/// produces a diagnostic attached to the offending operation's source
/// location and a failed result; the emitter never produces silently wrong
/// output.
///
/// \param op the root operation to translate; must not be null.
/// \param os the stream that receives the generated Rust source text.
/// \returns success if the whole tree was emitted, failure otherwise.
LogicalResult translateToRust(Operation *op, raw_ostream &os);

/// Registers the EmitRust-to-Rust translation with the global translation
/// registry under the command-line flag `--mlir-to-rust`, together with the
/// dialects required to parse its input. Intended to be called once from a
/// translate tool's `main` before invoking `mlirTranslateMain`.
void registerToRustTranslation();

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_TARGET_TRANSLATETORUST_H

//===- ImportC.h - C-to-EmitRust importer entry point -----------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file declares the public entry point of the C importer: a function
/// that parses a C source file with clang LibTooling and translates the
/// supported C subset into a hybrid MLIR module of core (func/arith/memref/
/// cf) and EmitRust dialect operations.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_IMPORTC_H
#define EMITRUST_IMPORTC_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"
#include "llvm/ADT/StringRef.h"

namespace mlir {
class MLIRContext;

namespace emitrust {

/// Imports the C source file at `path` into an MLIR module.
///
/// The file is parsed as C11 with clang. Scalar locals and control flow are
/// translated to core dialects (rank-0 `memref.alloca` cells, `arith`
/// operations, and `cf` branches) so that upstream passes (`--mem2reg`,
/// `--lift-cf-to-scf`) can recover structured, SSA-form IR. Aggregates and
/// pointers are translated directly to EmitRust place operations
/// (`emitrust.variable`/`member`/`subscript`/`deref`/`load`/`assign`/
/// `addr_of`), and complete named struct definitions become module-level
/// `emitrust.struct_def` operations. The C `main` function is renamed to
/// `c_main`.
///
/// The required dialects (emitrust, func, arith, memref, cf) are loaded into
/// `context` by this function. Every C construct outside the supported
/// subset produces a diagnostic carrying a `FileLineColLoc` source location;
/// no silently wrong IR is ever produced. The returned module has been
/// verified.
///
/// \param path the path of the C source file to import.
/// \param context the MLIR context that owns the created module.
/// \returns the imported module, or null on failure with diagnostics already
///          emitted through `context`'s diagnostic engine (parse errors are
///          printed to stderr by clang).
OwningOpRef<ModuleOp> importC(llvm::StringRef path, MLIRContext &context);

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_IMPORTC_H

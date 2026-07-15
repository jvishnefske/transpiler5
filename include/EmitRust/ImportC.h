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
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <string>

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
/// `extraClangArgs` are appended to the clang command line after `-std=c11`,
/// in the order given, so a caller can pass include-path flags (`-I<dir>`,
/// `-isystem <dir>`) and any other clang driver arguments. When empty, the
/// command line is exactly `-std=c11` plus the configured resource dir, so
/// this preserves the historical single-file behavior.
///
/// \param path the path of the C source file to import.
/// \param extraClangArgs additional clang command-line arguments.
/// \param context the MLIR context that owns the created module.
/// \returns the imported module, or null on failure with diagnostics already
///          emitted through `context`'s diagnostic engine (parse errors are
///          printed to stderr by clang).
OwningOpRef<ModuleOp> importC(llvm::StringRef path,
                              llvm::ArrayRef<std::string> extraClangArgs,
                              MLIRContext &context);

/// Convenience overload importing a single file with no extra clang args.
OwningOpRef<ModuleOp> importC(llvm::StringRef path, MLIRContext &context);

/// Imports a whole C project: parses every source file in `paths` as an
/// independent C11 translation unit and merges them into one MLIR module,
/// resolving external symbols across translation units and keeping
/// internal-linkage (`static`) symbols distinct.
///
/// Linkage model:
///  - External-linkage functions and globals keep their bare C name and are
///    unified program-wide; a prototype in one TU is satisfied by a
///    definition in another. Two definitions of the same external symbol, or
///    a referenced-but-undefined non-variadic external function/global, are
///    located diagnostics.
///  - Internal-linkage (`static` at file scope) functions and globals are
///    mangled with a per-TU prefix so identically named file-statics in
///    different TUs never collide.
///  - Aggregate definitions (`struct`/`enum`) shared through a header are
///    deduplicated by symbol name; a name reused with a different shape is a
///    located diagnostic.
///
/// \param paths the C source files to merge (at least one, all non-null ASTs).
/// \param extraClangArgs additional clang command-line arguments (include
///        paths etc.), applied to every translation unit.
/// \param context the MLIR context that owns the created module.
/// \returns the merged, verified module, or null on failure with diagnostics
///          already emitted.
OwningOpRef<ModuleOp> importCProject(llvm::ArrayRef<std::string> paths,
                                     llvm::ArrayRef<std::string> extraClangArgs,
                                     MLIRContext &context);

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_IMPORTC_H

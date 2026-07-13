//===- CrateEmitter.h - Pure Rust crate content rendering ------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file declares the functional core of the emitrust-cc driver: pure
/// functions that map a fully converted EmitRust module and a crate name to
/// the textual contents of a cargo crate (`Cargo.toml` and `src/main.rs`).
/// No function in this file touches the filesystem or any other side effect;
/// the imperative shell in emitrust-cc.cpp writes the returned strings out.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_TOOLS_EMITRUST_CC_CRATEEMITTER_H
#define EMITRUST_TOOLS_EMITRUST_CC_CRATEEMITTER_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Support/LLVM.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace emitrustcc {

/// Derives a cargo package name from an input file's basename stem.
///
/// Uppercase ASCII letters are lowered, characters outside `[a-z0-9_]` are
/// replaced with `_`, an empty stem becomes `transpiled`, and a leading
/// digit is prefixed with `_` (cargo rejects package names that start with a
/// digit). The result is always a nonempty string over `[a-z0-9_]` that does
/// not start with a digit, e.g. `loops.c` -> `loops`, `emit-crate.c` ->
/// `emit_crate`.
///
/// \param stem the input basename without its extension.
/// \returns the sanitized crate name.
std::string sanitizeCrateName(llvm::StringRef stem);

/// Returns true if `module` defines an `emitrust.func` named `c_main`, the
/// symbol the importer gives the C `main` function.
///
/// \param module the module to inspect; must be a verified EmitRust module.
/// \returns true when a `c_main` EmitRust function is present.
bool hasCMain(mlir::ModuleOp module);

/// Renders the complete contents of the crate's `Cargo.toml`.
///
/// The manifest is minimal: a `[package]` table with the given name, version
/// `0.1.0`, and edition `2021`. There are no dependencies and the release
/// profile is left at its defaults (release optimization is load-bearing for
/// differential testing: debug Rust panics on integer overflow where C
/// wraps).
///
/// \param crateName the sanitized package name.
/// \returns the manifest text.
std::string renderCargoToml(llvm::StringRef crateName);

/// Renders the Rust source for `module`.
///
/// When the module defines `c_main` the result is the exact text written to
/// a crate's `src/main.rs`: an `#![allow(...)]` header that keeps the
/// emitter's statement-per-op style warning-clean, the `translateToRust`
/// output of the module, and a verbatim
/// `fn main() { std::process::exit(c_main()); }` wrapper. When `c_main` is
/// absent the result is the bare translation with neither header nor
/// wrapper.
///
/// \param module the fully converted EmitRust module to translate.
/// \returns the Rust source text, or failure with diagnostics already
///          emitted through the module's context.
mlir::FailureOr<std::string> renderRustSource(mlir::ModuleOp module);

} // namespace emitrustcc

#endif // EMITRUST_TOOLS_EMITRUST_CC_CRATEEMITTER_H

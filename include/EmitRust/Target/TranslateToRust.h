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

/// Knobs that change the emitted Rust WITHOUT changing what it means.
///
/// The default-constructed value reproduces the historical output byte for
/// byte, so every caller that does not care — `emitrust-translate`'s
/// `--mlir-to-rust`, and `emitrust-cc` whenever it emits a BINARY crate — is
/// unaffected by this struct existing.
struct RustEmitOptions {
  /// FR-51: emit `pub` on the items a LIBRARY crate must export.
  ///
  /// A binary crate's items are private and the crate's `#![allow(dead_code)]`
  /// header keeps that warning-clean; a library whose items are all private
  /// exports nothing and is useless. When this is set the emitter marks:
  ///
  ///  - every `emitrust.func` — free function or `emitrust.impl` method —
  ///    whose symbol does NOT carry an internal-linkage marker
  ///    (`isInternalLinkageSymbolName`, `EmitRust/CSymbolLinkage.h`). A C
  ///    `static` function was TU-private by the author's choice and stays
  ///    private in the crate;
  ///  - every `emitrust.struct_def` and `emitrust.enum_def`, with their
  ///    fields, tuple element and variant constants. These are exported
  ///    UNCONDITIONALLY, for two reasons: Rust's private-in-public rule
  ///    (E0446) means a type named in an exported signature must itself be
  ///    exported, and a C record definition carries no linkage of its own —
  ///    it lives in a header, the importer gives it no per-TU tag, and the
  ///    FR-40 item graph does not model one either. So exporting them cannot
  ///    leak anything that C kept private.
  ///
  /// `emitrust.global` is NEVER exported. Module-level mutable state is not
  /// an API a Rust caller can use (each global is a `static` or a
  /// `thread_local!` `Cell`), and global names are not a sound linkage
  /// oracle in the first place: a FUNCTION-local `static` is mangled
  /// `<function>_<name>` and so inherits its enclosing function's tag rather
  /// than one of its own, which would export a variable that was never even
  /// file-scope in C.
  bool exportItems = false;

  /// FR-139: additionally give every ALL-SCALAR exported function the C ABI:
  /// `#[no_mangle] pub extern "C" fn` instead of `pub fn`.
  ///
  /// Meaningful only together with `exportItems` (the emitter never gives a
  /// C-ABI symbol to an item it is not exporting in the first place), and
  /// opt-in: default-constructed, this reproduces the pre-FR-139 output byte
  /// for byte, which is what keeps every `--emit=crate` golden pinned.
  ///
  /// The reason it exists: a dlopen/dlsym host — the shape the TRACTOR
  /// corpus's `cando` harness takes, and the shape any C consumer of a
  /// transpiled library takes — can only call a BARE C symbol through a C
  /// signature. An rlib of Rust-ABI, Rust-mangled `pub fn` is unreachable
  /// from one, whatever the quality of the translation.
  ///
  /// ALL-SCALAR is the whole safety argument and MUST NOT be loosened. Every
  /// input and every result of the function type has to be a BUILTIN
  /// `IntegerType` or `FloatType`; every EmitRust dialect type disqualifies.
  /// An all-scalar `extern "C"` entry point needs no `unsafe` and no shim, so
  /// the C declaration and the Rust definition agree by construction. A
  /// `&[T]` parameter, by contrast, is a two-register fat pointer that rustc
  /// accepts under `extern "C"` with only a non-FFI-safe WARNING and that
  /// then shifts every later argument — measured across the boundary on the
  /// real crc16 case (clang native 27235, cdylib 0). That is the
  /// silently-wrong-code class this project forbids, so a non-scalar
  /// signature keeps its plain `pub fn` and is REPORTED with a located
  /// warning rather than exported wrongly or dropped in silence.
  bool cAbiExports = false;
};

/// Translates `op` to Rust source code written to `os` under `options`.
///
/// \param op the root operation to translate; must not be null.
/// \param os the stream that receives the generated Rust source text.
/// \param options the emission knobs; see `RustEmitOptions`.
/// \returns success if the whole tree was emitted, failure otherwise.
LogicalResult translateToRust(Operation *op, raw_ostream &os,
                              const RustEmitOptions &options);

/// Registers the EmitRust-to-Rust translation with the global translation
/// registry under the command-line flag `--mlir-to-rust`, together with the
/// dialects required to parse its input. Intended to be called once from a
/// translate tool's `main` before invoking `mlirTranslateMain`.
void registerToRustTranslation();

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_TARGET_TRANSLATETORUST_H

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
/// the textual contents of a cargo crate (`Cargo.toml` and the crate root,
/// `src/main.rs` or `src/lib.rs`). No function in this file touches the
/// filesystem or any other side effect; the imperative shell in
/// emitrust-cc.cpp writes the returned strings out.
///
/// # FR-51: binary crates and library crates
///
/// A C project either has an entry point or it does not, and most real C and
/// C++ code — every library — does not. The crate SHAPE follows that fact:
/// a module defining `c_main` becomes a BINARY crate exactly as it always
/// has (`src/main.rs`, a `fn main` wrapper forwarding the exit code), and a
/// module without one becomes a LIBRARY crate (`src/lib.rs`, no wrapper, a
/// `[lib]` section in the manifest, and `pub` on the items a caller outside
/// the crate must be able to reach).
///
/// The rule is a function of the module, not of the command line, so
/// `--crate-type=auto` is the default and needs no thought; the explicit
/// `bin`/`lib` settings exist for the cases where the user knows better than
/// the heuristic, and forcing `bin` on a module with no `c_main` is a clean
/// error rather than a crate that cannot link.
///
/// Why this matters beyond libraries: under `--incremental` (FR-44) the
/// importer is DESIGNED to drop items it cannot translate, and when the
/// dropped item happens to be `main` the old hard failure destroyed the
/// whole point of the flag — no crate, no PORTING.md, no per-item
/// accounting, i.e. exactly the all-or-nothing outcome `--incremental`
/// exists to remove. Such a project now yields a library crate and its full
/// report.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_TOOLS_EMITRUST_CC_CRATEEMITTER_H
#define EMITRUST_TOOLS_EMITRUST_CC_CRATEEMITTER_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Support/LLVM.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace emitrustcc {

/// FR-51: the two shapes a cargo crate can have.
enum class CrateType {
  /// `src/main.rs` plus a `fn main` wrapper; the crate builds to an
  /// executable. Requires the module to define `c_main`.
  Bin,
  /// `src/lib.rs` with a `[lib]` manifest section and no wrapper; the crate
  /// builds to a library and exports its external-linkage items.
  Lib,
};

/// What `--crate-type` was set to, before the module has been consulted.
enum class CrateTypeRequest {
  /// Decide from the module: `Bin` if it defines `c_main`, else `Lib`.
  Auto,
  /// Force a binary crate; an error when the module defines no `c_main`.
  Bin,
  /// Force a library crate, even for a module that defines `c_main` (which
  /// then becomes an ordinary exported function rather than an entry point).
  Lib,
};

/// Resolves `request` against `module` (FR-51).
///
/// `Auto` selects `Lib` exactly when the module has no `c_main`, so the
/// historical behaviour — every module WITH a `main` emits the same binary
/// crate, byte for byte — is unchanged, and the case that used to be a hard
/// failure is the only one whose result is new.
///
/// This function does not diagnose the one invalid combination
/// (`CrateTypeRequest::Bin` with no `c_main`); it returns `Bin` there, and
/// the caller reports it with a location. Keeping the diagnostic out of the
/// functional core is the same split the rest of this file follows.
///
/// \param request the resolved `--crate-type` option.
/// \param module the fully converted module.
/// \returns the crate shape to emit.
CrateType selectCrateType(CrateTypeRequest request, mlir::ModuleOp module);

/// The crate-root source file name for `type`: `main.rs` or `lib.rs`.
///
/// \param type the crate shape.
/// \returns the basename, without the `src/` directory.
llvm::StringRef crateRootFileName(CrateType type);

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
/// A `CrateType::Lib` manifest additionally carries a `[lib]` section naming
/// the library and its crate root. Cargo would infer both from the presence
/// of `src/lib.rs`, but the section is written out anyway so that reading the
/// manifest alone answers "what kind of crate is this?" — which is precisely
/// the question FR-51 introduced, and the one a consuming build system asks.
/// A `CrateType::Bin` manifest is byte-identical to the pre-FR-51 output.
///
/// \param crateName the sanitized package name.
/// \param type the crate shape.
/// \returns the manifest text.
std::string renderCargoToml(llvm::StringRef crateName, CrateType type);

/// Renders the crate-root Rust source for `module` (`src/main.rs` for a
/// binary crate, `src/lib.rs` for a library one).
///
/// Both shapes begin with the `#![allow(...)]` header that keeps the
/// emitter's statement-per-op style warning-clean, followed by the
/// `translateToRust` output of the module. A `CrateType::Bin` root then ends
/// with a verbatim `fn main() { std::process::exit(c_main()); }` wrapper (or
/// its argc-passing variant) and is byte-identical to the pre-FR-51 output.
/// A `CrateType::Lib` root has no wrapper and is translated with
/// `RustEmitOptions::exportItems`, so its external-linkage functions and its
/// types are `pub`.
///
/// \param module the fully converted EmitRust module to translate.
/// \param type the crate shape.
/// \returns the Rust source text, or failure with diagnostics already
///          emitted through the module's context.
mlir::FailureOr<std::string> renderCrateRoot(mlir::ModuleOp module,
                                             CrateType type);

/// FR-59: `renderCrateRoot` for a WORKSPACE MEMBER that depends on
/// `depCrates`. With an empty list this is byte-for-byte the overload
/// above (the no-partition invariant); otherwise the `#![allow]` header
/// additionally allows `unused_imports` (a member gets every dependency
/// it references anywhere, not per-item) and one `use <dep>::*;` line per
/// dependency follows the header, which is how cross-crate references —
/// emitted as bare names — resolve against the FR-51 exports.
///
/// \param module the member's converted module slice.
/// \param type the member's crate shape.
/// \param depCrates the package names of the member's path dependencies.
/// \returns the Rust source text, or failure with diagnostics emitted.
mlir::FailureOr<std::string>
renderCrateRoot(mlir::ModuleOp module, CrateType type,
                llvm::ArrayRef<std::string> depCrates);

/// FR-59: the member manifest — `renderCargoToml` plus a `[dependencies]`
/// table of path dependencies (`<dep> = { path = "../<dep>" }`), one per
/// entry, in order. With no deps this is byte-for-byte `renderCargoToml`.
///
/// \param crateName the member's package name.
/// \param type the member's crate shape.
/// \param depCrates the package names of the member's dependencies.
/// \returns the manifest text.
std::string renderMemberCargoToml(llvm::StringRef crateName, CrateType type,
                                  llvm::ArrayRef<std::string> depCrates);

/// FR-59: the virtual workspace root manifest: a `[workspace]` table with
/// resolver 2 and the members in plan order. The root carries no
/// `[package]` — the binary member is an ordinary member, and the shared
/// `target/` directory lands beside this manifest.
///
/// \param members the member directory/package names, in plan order.
/// \returns the manifest text.
std::string renderWorkspaceToml(llvm::ArrayRef<std::string> members);

} // namespace emitrustcc

#endif // EMITRUST_TOOLS_EMITRUST_CC_CRATEEMITTER_H

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
#include "llvm/ADT/SmallVector.h"
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

/// FR-139: is `name` usable verbatim as a cargo package / `[lib]` name?
///
/// True for a nonempty string over `[A-Za-z0-9_]` that does not start with a
/// digit. `sanitizeCrateName` maps every OTHER spelling into this set, so an
/// EXPLICIT `--crate-name` that already satisfies this can pass through
/// untouched — which is how a CamelCase library name (`libSieve.so`) survives
/// to the manifest, where the sanitizer's lowercasing used to destroy it.
/// DERIVED names (an input stem, an output-directory stem) always go through
/// the sanitizer regardless.
///
/// \param name the candidate crate name.
/// \returns true when the name needs no sanitization.
bool isUsableCrateName(llvm::StringRef name);

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

/// FR-62 slice 5c: returns true if `module` carries any
/// `emitrust.actor_runtime` anchor in mode `async` — the trigger for the
/// tokio crate flavor (the async `fn main` shim in the crate root and the
/// unconditional tokio dependency in the manifest). Anchor-derived rather
/// than flag-derived so the manifest exactly tracks what the crate root
/// references: a `--actor-mode=async` run whose every actor demoted emits
/// a crate with no tokio reference, and its manifest stays the default —
/// the offline contract (E4) is preserved for crates that need nothing.
///
/// \param module the fully converted module about to be rendered.
/// \returns true when any async-mode actor runtime anchor is present.
bool hasAsyncActorRuntime(mlir::ModuleOp module);

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
/// FR-62 slice 5c: `asyncActorRuntime` (default false) selects the ASYNC
/// crate flavor's manifest — E4's posture: never a cargo feature (a
/// default-off optional dependency already breaks `cargo build --offline`,
/// the measured NO-GO), but an UNCONDITIONAL `[dependencies]` entry
/// `tokio = { version = "1", features = ["rt", "sync"] }` appended after
/// the same `[package]`/`[lints.rust]` tables, which are byte-identical to
/// the default manifest. The feature list is measured, not E4's recorded
/// guess: the explicit `Builder::new_current_thread()` main shim needs no
/// "macros" (3 crates in the dependency tree vs 10 with the attribute
/// macro). Its offline build fails loudly at resolution — the correct
/// failure direction.
///
/// FR-139: `cAbiExports` (default false) adds `crate-type = ["cdylib"]` to
/// the `[lib]` section, so cargo builds a real shared object a C host can
/// dlopen. It is meaningful only for `CrateType::Lib` and is ignored for a
/// binary crate, whose manifest has no `[lib]` section at all.
///
/// FR-139, independently of that flag: the `non_snake_case = "deny"` line is
/// OMITTED when `crateName` would itself trip the lint. rustc applies
/// `non_snake_case` to the CRATE NAME too, so the emitted manifest was
/// vetoing names the caller explicitly asked for
/// (`error: crate 'Sieve' should have a snake case name`, reproduced) — and a
/// CamelCase `[lib] name` is exactly what a host that dlopens `libSieve.so`
/// requires. The relaxation is as narrow as the problem: only that one lint,
/// only for a name that would fail, and only ever for the name — the deny
/// still governs every identifier the emitter itself produces.
///
/// \param crateName the package name.
/// \param type the crate shape.
/// \param asyncActorRuntime append the async flavor's tokio dependency.
/// \param cAbiExports build the library as a cdylib.
/// \returns the manifest text.
std::string renderCargoToml(llvm::StringRef crateName, CrateType type,
                            bool asyncActorRuntime = false,
                            bool cAbiExports = false);

/// Renders the crate-root Rust source for `module` (`src/main.rs` for a
/// binary crate, `src/lib.rs` for a library one).
///
/// Both shapes begin with the `#![allow(...)]` header that keeps the
/// emitter's statement-per-op style warning-clean, followed by the
/// `translateToRust` output of the module. A `CrateType::Bin` root then ends
/// with a verbatim `fn main() { std::process::exit(c_main()); }` wrapper (or
/// its argc-passing variant) and is byte-identical to the pre-FR-51 output.
/// FR-62 slice 5c: when the module carries an async actor runtime anchor,
/// `c_main` is an `async fn` and the wrapper is the current_thread shim —
/// `tokio::runtime::Builder::new_current_thread().enable_all().build()`
/// driving `block_on(c_main())` — chosen over `#[tokio::main]` because it
/// needs no "macros" feature (measured: 3 dependency-tree crates vs 10).
/// A `CrateType::Lib` root has no wrapper and is translated with
/// `RustEmitOptions::exportItems`, so its external-linkage functions and its
/// types are `pub`.
///
/// FR-139: `cAbiExports` (default false) additionally emits every ALL-SCALAR
/// exported function as `#[no_mangle] pub extern "C" fn`, so a C host can
/// dlsym the bare symbol; a non-scalar exported signature keeps its `pub fn`
/// and is reported with a located warning (see `RustEmitOptions::cAbiExports`
/// for why that restriction is not negotiable). The flag rides on top of the
/// library export set and does nothing for a binary crate.
///
/// \param module the fully converted EmitRust module to translate.
/// \param type the crate shape.
/// \param cAbiExports give all-scalar exports the C ABI and a bare symbol.
/// \returns the Rust source text, or failure with diagnostics already
///          emitted through the module's context.
mlir::FailureOr<std::string> renderCrateRoot(mlir::ModuleOp module,
                                             CrateType type,
                                             bool cAbiExports = false);

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
/// \param cAbiExports FR-139: give all-scalar exports the C ABI. The driver
///        refuses `--c-abi-exports` under `--partition` (a workspace member is
///        a path dependency of its siblings, which a cdylib cannot be), so
///        this is false on every workspace path today.
/// \returns the Rust source text, or failure with diagnostics emitted.
mlir::FailureOr<std::string>
renderCrateRoot(mlir::ModuleOp module, CrateType type,
                llvm::ArrayRef<std::string> depCrates,
                bool cAbiExports = false);

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

/// FR-160: what became of one requested test entry point.
///
/// The functional core decides the fate and records it here; the imperative
/// shell turns a nonempty `skipReason` into a LOCATED warning. That split is
/// the same one `selectCrateType` follows, and it is what lets a whole-project
/// entries file be applied to one translation unit: a symbol this module does
/// not define is not an error, it belongs to a different unit.
struct TestEntryReport {
  /// The symbol as requested.
  std::string symbol;
  /// Empty when a `#[test]` was emitted; otherwise why it was not.
  std::string skipReason;
  /// Set when a test WAS emitted but carries `#[ignore = ...]` -- a recovered
  /// stub, whose body is an `unimplemented!`. The gap stays visible in
  /// `cargo test` output instead of vanishing.
  std::string ignoreReason;
  /// The function, when one was found, for the diagnostic's location.
  mlir::Operation *op = nullptr;
};

/// FR-160: renders the `#[cfg(test)] mod emitrust_tests` block wrapping each
/// of `entries` that can be called, or "" when none can.
///
/// A C project's test suite is an oracle the emitter otherwise throws away.
/// This turns each named entry point into a `#[test]`, so `cargo test` green
/// means the C suite's own assertions hold in the transpiled code -- a
/// DIFFERENTIAL oracle over code no hand-written EndToEnd test will cover.
///
/// The shape follows what meson and CTest already agree on: a test passes iff
/// it exits 0. So an entry returning an integer becomes
/// `assert_eq!(sym(), 0)`, and one returning nothing is run for its panics --
/// a weaker oracle, but not a vacuous one, because a transpiled body's
/// failure mode IS a panic (a bounds check, a null function pointer, an
/// `unimplemented!`).
///
/// NEVER A VACUOUS PASS is the governing rule. An entry is SKIPPED, not
/// wrapped, when calling it would prove nothing or would not compile:
///   - no function of that name in this module (it belongs to another unit);
///   - it takes arguments (nothing models argv, and meson's own registry says
///     333 of systemd's 337 one-TU tests take none);
///   - it is generic over the `Externals` trait, i.e. its callees are
///     undefined in a solo-TU import -- a test that panics inside
///     `Externals::x` proves nothing. `--link` the shards and the trait, and
///     this restriction, both disappear;
///   - it rendered as a method of an impl block (the FR-62 actor lift moves an
///     arm that touches a file-local global into one), so it is not callable
///     as a free function;
///   - it returns something other than an integer or nothing.
/// A recovered STUB is the one case that is emitted anyway, `#[ignore]`d with
/// its rejection diagnostic, because "this test exists and does not run yet"
/// is information and silence is not.
///
/// The generated module reaches its subjects through `super::`, so a
/// file-local (non-`pub`) function -- which is what every `static` test body
/// imports as -- is wrappable without changing its visibility.
///
/// \param module the fully converted module about to be rendered.
/// \param entries the requested entry-point symbols, in order.
/// \param reports out: one entry per request, in the same order.
/// \returns the Rust text to append to the crate root, or "" if empty.
std::string renderTestModule(mlir::ModuleOp module,
                             llvm::ArrayRef<std::string> entries,
                             llvm::SmallVectorImpl<TestEntryReport> &reports);

} // namespace emitrustcc

#endif // EMITRUST_TOOLS_EMITRUST_CC_CRATEEMITTER_H

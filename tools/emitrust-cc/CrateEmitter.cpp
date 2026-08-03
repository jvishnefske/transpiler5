//===- CrateEmitter.cpp - Pure Rust crate content rendering --------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file implements the functional core of the emitrust-cc driver: the
/// crate-name sanitizer, the FR-51 crate-shape selection, and the pure
/// renderers that turn a fully converted EmitRust module into `Cargo.toml`
/// and crate-root (`src/main.rs` or `src/lib.rs`) text.
//
//===----------------------------------------------------------------------===//

#include "CrateEmitter.h"

#include "EmitRust/CSymbolNaming.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/Target/TranslateToRust.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace emitrustcc {

/// Attribute header prepended to every generated crate root. Under the default
/// idiomatic rename only two lints are allowed, both because they are intrinsic
/// to a faithful transpile rather than masking sloppy codegen:
///   - `dead_code`: rejected items intentionally keep their `struct_def` /
///     `global` definitions (dropping them was measured and rejected as risking
///     dangling symbols), and a binary crate legitimately holds unreferenced
///     imported items.
///   - `unused_assignments`: the emitter's definite-assignment/dead-store
///     analysis eliminates these everywhere it can prove safe (byte-diff
///     verified); the sole residual is a dead store inside a loop, whose sound
///     cross-iteration liveness would risk a miscompile and is deliberately not
///     attempted. It is a per-crate no-op for every input in the suite except
///     one.
/// Every other lint the old blanket header silenced is now DENIED in
/// `Cargo.toml`'s `[lints.rust]` table (see `renderCargoToml`), so a regression
/// fails the build.
///
/// Under `--preserve-c-names` the three naming lints join the allow list
/// instead of the deny table: verbatim C spellings legitimately trip them, and
/// the flag's whole point is to keep those spellings.
static constexpr llvm::StringLiteral kAllowHeader =
    "#![allow(dead_code, unused_assignments)]\n";
static constexpr llvm::StringLiteral kAllowHeaderPreserveNames =
    "#![allow(dead_code, unused_assignments, non_snake_case, "
    "non_upper_case_globals, non_camel_case_types)]\n";

/// Verbatim entry-point wrapper: forwards the imported C `main`'s return
/// value as the process exit code.
static constexpr llvm::StringLiteral kMainWrapper =
    "fn main() { std::process::exit(c_main()); }\n";

/// Wrapper for a C `main(int argc, char **argv)` imported with its `argv`
/// dropped: the process argument count (program name included, matching
/// C's argc) is passed as the sole parameter. `args_os` is used so an
/// argument that is not valid Unicode still counts (`args` would panic).
static constexpr llvm::StringLiteral kMainArgcWrapper =
    "fn main() { std::process::exit(c_main(std::env::args_os().len() as "
    "i32)); }\n";

/// FR-62 slice 5c: the async flavor's entry wrappers — `c_main` renders as
/// an `async fn` when an async actor runtime anchor exists, so the shim
/// builds a tokio CURRENT_THREAD runtime (E4's determinism substrate:
/// single-threaded scheduling + the wrappers' immediate await keep effect
/// order equal to program order) and drives `c_main` to completion with
/// `block_on`. The explicit Builder form is chosen over
/// `#[tokio::main(flavor = "current_thread")]` because it works without
/// the "macros" feature: both were probe-built, and the attribute macro
/// pulls tokio-macros/syn/quote/proc-macro2 (10 crates in the tree vs 3)
/// for zero behavioral difference.
static constexpr llvm::StringLiteral kMainWrapperAsync =
    "fn main() { "
    "std::process::exit(tokio::runtime::Builder::new_current_thread()"
    ".enable_all().build().expect(\"tokio runtime build failed\")"
    ".block_on(c_main())); }\n";

/// The async shim for a `c_main` that takes the imported argc parameter.
static constexpr llvm::StringLiteral kMainArgcWrapperAsync =
    "fn main() { "
    "std::process::exit(tokio::runtime::Builder::new_current_thread()"
    ".enable_all().build().expect(\"tokio runtime build failed\")"
    ".block_on(c_main(std::env::args_os().len() as i32))); }\n";

CrateType selectCrateType(CrateTypeRequest request, mlir::ModuleOp module) {
  switch (request) {
  case CrateTypeRequest::Bin:
    return CrateType::Bin;
  case CrateTypeRequest::Lib:
    return CrateType::Lib;
  case CrateTypeRequest::Auto:
    return hasCMain(module) ? CrateType::Bin : CrateType::Lib;
  }
  llvm_unreachable("covered switch");
}

llvm::StringRef crateRootFileName(CrateType type) {
  return type == CrateType::Bin ? "main.rs" : "lib.rs";
}

std::string sanitizeCrateName(llvm::StringRef stem) {
  std::string name;
  name.reserve(stem.size());
  for (char c : stem) {
    if (c >= 'A' && c <= 'Z')
      name.push_back(static_cast<char>(c - 'A' + 'a'));
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
      name.push_back(c);
    else
      name.push_back('_');
  }
  if (name.empty())
    name = "transpiled";
  if (name.front() >= '0' && name.front() <= '9')
    name.insert(name.begin(), '_');
  return name;
}

bool hasCMain(mlir::ModuleOp module) {
  mlir::Operation *symbol =
      mlir::SymbolTable::lookupSymbolIn(module, "c_main");
  return symbol != nullptr && llvm::isa<mlir::emitrust::FuncOp>(symbol);
}

bool hasAsyncActorRuntime(mlir::ModuleOp module) {
  for (auto runtime : module.getOps<mlir::emitrust::ActorRuntimeOp>())
    if (runtime.getMode() == mlir::emitrust::ActorMode::async)
      return true;
  return false;
}

/// Returns whether the module's `c_main` takes the imported argc
/// parameter (a C `main(int argc, char **argv)`; `argv` is dropped at
/// import). Callers have already established `hasCMain`.
static bool cMainTakesArgc(mlir::ModuleOp module) {
  auto funcOp = llvm::dyn_cast_if_present<mlir::emitrust::FuncOp>(
      mlir::SymbolTable::lookupSymbolIn(module, "c_main"));
  return funcOp && funcOp.getFunctionType().getNumInputs() == 1;
}

std::string renderCargoToml(llvm::StringRef crateName, CrateType type,
                            bool asyncActorRuntime) {
  std::string toml;
  llvm::raw_string_ostream os(toml);
  os << "[package]\n"
     << "name = \"" << crateName << "\"\n"
     << "version = \"0.1.0\"\n"
     << "edition = \"2021\"\n";
  if (type == CrateType::Lib)
    os << "\n"
       << "[lib]\n"
       << "name = \"" << crateName << "\"\n"
       << "path = \"src/lib.rs\"\n";
  // FR-53: the lints the old blanket allow header silenced are now DENIED, so
  // any regression in the emitter's warning-clean codegen fails `cargo build`.
  // `dead_code` and `unused_assignments` stay allowed in the crate root (see
  // `kAllowHeader`); everything else must be clean. The three naming lints are
  // denied only under the idiomatic rename -- `--preserve-c-names` keeps
  // verbatim C spellings, which legitimately trip them (allowed in the
  // header instead).
  os << "\n"
     << "[lints.rust]\n"
     << "unused_variables = \"deny\"\n"
     << "unused_mut = \"deny\"\n"
     << "unused_parens = \"deny\"\n"
     << "unpredictable_function_pointer_comparisons = \"deny\"\n";
  if (mlir::emitrust::idiomaticRenameEnabled())
    os << "non_snake_case = \"deny\"\n"
       << "non_upper_case_globals = \"deny\"\n"
       << "non_camel_case_types = \"deny\"\n";
  // FR-62 slice 5c: the ASYNC crate flavor (E4) appends its tokio
  // dependency UNCONDITIONALLY — never behind a cargo feature, whose mere
  // declaration breaks `cargo build --offline` (the measured NO-GO) — and
  // everything above stays byte-identical to the default manifest. The
  // feature list is the measured minimum for the emitted shape: "rt"
  // (Builder + task::spawn), "sync" (mpsc + oneshot); no "macros" because
  // the main shim is the explicit Builder, not the attribute macro.
  if (asyncActorRuntime)
    os << "\n"
       << "[dependencies]\n"
       << "tokio = { version = \"1\", features = [\"rt\", \"sync\"] }\n";
  return toml;
}

mlir::FailureOr<std::string> renderCrateRoot(mlir::ModuleOp module,
                                             CrateType type) {
  return renderCrateRoot(module, type, /*depCrates=*/{});
}

mlir::FailureOr<std::string>
renderCrateRoot(mlir::ModuleOp module, CrateType type,
                llvm::ArrayRef<std::string> depCrates) {
  const bool wrapMain = type == CrateType::Bin;
  mlir::emitrust::RustEmitOptions emitOptions;
  // FR-51: only a library crate exports anything. A binary crate's items stay
  // private, which is both what they were and what keeps this rendering
  // byte-identical to every crate emitted before FR-51.
  emitOptions.exportItems = type == CrateType::Lib;
  std::string source;
  llvm::raw_string_ostream os(source);
  llvm::StringRef header = mlir::emitrust::idiomaticRenameEnabled()
                               ? kAllowHeader
                               : kAllowHeaderPreserveNames;
  if (depCrates.empty()) {
    os << header << "\n";
  } else {
    // FR-59 workspace member: the allow list additionally admits
    // unused_imports (a member gets every dependency it references
    // anywhere, not per item), and the glob imports follow — they are how
    // the emitter's bare cross-crate names resolve against FR-51's pubs.
    llvm::StringRef closer = ")]\n";
    os << header.drop_back(closer.size()) << ", unused_imports" << closer;
    for (const std::string &dep : depCrates)
      os << "use " << dep << "::*;\n";
    os << "\n";
  }
  if (mlir::failed(mlir::emitrust::translateToRust(module, os, emitOptions)))
    return mlir::failure();
  if (wrapMain) {
    // FR-62 slice 5c: an async actor runtime anchor means `c_main` rendered
    // as an `async fn`, so the wrapper is the tokio current_thread shim.
    const bool asyncMain = hasAsyncActorRuntime(module);
    os << "\n"
       << (cMainTakesArgc(module)
               ? (asyncMain ? kMainArgcWrapperAsync : kMainArgcWrapper)
               : (asyncMain ? kMainWrapperAsync : kMainWrapper));
  }
  return source;
}

std::string renderMemberCargoToml(llvm::StringRef crateName, CrateType type,
                                  llvm::ArrayRef<std::string> depCrates) {
  std::string toml = renderCargoToml(crateName, type);
  if (depCrates.empty())
    return toml;
  llvm::raw_string_ostream os(toml);
  os << "\n[dependencies]\n";
  for (const std::string &dep : depCrates)
    os << dep << " = { path = \"../" << dep << "\" }\n";
  return toml;
}

std::string renderWorkspaceToml(llvm::ArrayRef<std::string> members) {
  std::string toml;
  llvm::raw_string_ostream os(toml);
  os << "[workspace]\n"
     << "resolver = \"2\"\n"
     << "members = [";
  for (auto [index, member] : llvm::enumerate(members))
    os << (index ? ", " : "") << "\"" << member << "\"";
  os << "]\n";
  return toml;
}

} // namespace emitrustcc

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

#include "EmitRust/EmitRustOps.h"
#include "EmitRust/Target/TranslateToRust.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace emitrustcc {

/// Attribute header prepended to every generated crate root. Only two lints are
/// allowed, both because they are intrinsic to a faithful transpile rather than
/// masking sloppy codegen:
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
static constexpr llvm::StringLiteral kAllowHeader =
    "#![allow(dead_code, unused_assignments)]\n";

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

/// Returns whether the module's `c_main` takes the imported argc
/// parameter (a C `main(int argc, char **argv)`; `argv` is dropped at
/// import). Callers have already established `hasCMain`.
static bool cMainTakesArgc(mlir::ModuleOp module) {
  auto funcOp = llvm::dyn_cast_if_present<mlir::emitrust::FuncOp>(
      mlir::SymbolTable::lookupSymbolIn(module, "c_main"));
  return funcOp && funcOp.getFunctionType().getNumInputs() == 1;
}

std::string renderCargoToml(llvm::StringRef crateName, CrateType type) {
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
  // `kAllowHeader`); everything else must be clean.
  os << "\n"
     << "[lints.rust]\n"
     << "unused_variables = \"deny\"\n"
     << "unused_mut = \"deny\"\n"
     << "unused_parens = \"deny\"\n"
     << "unpredictable_function_pointer_comparisons = \"deny\"\n"
     << "non_snake_case = \"deny\"\n"
     << "non_upper_case_globals = \"deny\"\n"
     << "non_camel_case_types = \"deny\"\n";
  return toml;
}

mlir::FailureOr<std::string> renderCrateRoot(mlir::ModuleOp module,
                                             CrateType type) {
  const bool wrapMain = type == CrateType::Bin;
  mlir::emitrust::RustEmitOptions emitOptions;
  // FR-51: only a library crate exports anything. A binary crate's items stay
  // private, which is both what they were and what keeps this rendering
  // byte-identical to every crate emitted before FR-51.
  emitOptions.exportItems = type == CrateType::Lib;
  std::string source;
  llvm::raw_string_ostream os(source);
  os << kAllowHeader << "\n";
  if (mlir::failed(mlir::emitrust::translateToRust(module, os, emitOptions)))
    return mlir::failure();
  if (wrapMain)
    os << "\n" << (cMainTakesArgc(module) ? kMainArgcWrapper : kMainWrapper);
  return source;
}

} // namespace emitrustcc

//===- CrateEmitter.cpp - Pure Rust crate content rendering --------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file implements the functional core of the emitrust-cc driver: the
/// crate-name sanitizer and the pure renderers that turn a fully converted
/// EmitRust module into `Cargo.toml` and `src/main.rs` text.
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

/// Attribute header prepended to every generated `main.rs`. The emitter's
/// statement-per-op, mut-let style legitimately triggers these lints (for
/// example a `let mut` that is assigned in only one `if` arm), global
/// variables keep their original C spelling rather than SCREAMING_CASE,
/// struct names keep their C spelling too (including the synthesized
/// `Owner_<fn>_<base>` owner structs), and C function-pointer null/equality
/// tests compare `Option<fn>` values (the comparison is exact for the null
/// case C cares about), so the generated crate silences them to stay
/// warning-clean.
static constexpr llvm::StringLiteral kAllowHeader =
    "#![allow(unused_variables, unused_assignments, unused_mut, "
    "unused_parens, dead_code, non_upper_case_globals, "
    "non_camel_case_types, unpredictable_function_pointer_comparisons)]\n";

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

std::string renderCargoToml(llvm::StringRef crateName) {
  std::string toml;
  llvm::raw_string_ostream os(toml);
  os << "[package]\n"
     << "name = \"" << crateName << "\"\n"
     << "version = \"0.1.0\"\n"
     << "edition = \"2021\"\n";
  return toml;
}

mlir::FailureOr<std::string> renderRustSource(mlir::ModuleOp module) {
  const bool wrapMain = hasCMain(module);
  std::string source;
  llvm::raw_string_ostream os(source);
  if (wrapMain)
    os << kAllowHeader << "\n";
  if (mlir::failed(mlir::emitrust::translateToRust(module, os)))
    return mlir::failure();
  if (wrapMain)
    os << "\n" << (cMainTakesArgc(module) ? kMainArgcWrapper : kMainWrapper);
  return source;
}

} // namespace emitrustcc

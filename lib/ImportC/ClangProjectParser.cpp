//===- ClangProjectParser.cpp - clang parse shell for a project -*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements `EmitRust/ClangProjectParser.h`: `buildCommandLine`,
/// `isCxxSourcePath`, the `PerFileCompilationDatabase`, and the
/// `ClangTool::buildASTs` call they exist to feed. Moved out of ImportC.cpp
/// (where they lived in an anonymous namespace) by pure code motion when
/// FR-40's item graph became a second caller; the only edits are the loss
/// of the anonymous namespace and the addition of `buildProjectASTs`, which
/// is the three lines `importC`/`importCProject` used to spell inline.
///
/// It stays inside MLIREmitRustImportC rather than becoming its own library
/// for one concrete reason: the `EMITRUST_CLANG_RESOURCE_DIR` compile
/// definition is set on that target by lib/ImportC/CMakeLists.txt's
/// configure-time `clang -print-resource-dir` probe. Moving this file
/// elsewhere would either duplicate that probe or silently drop the
/// resource dir, breaking `<stdint.h>` and friends.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/ClangProjectParser.h"

#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/Support/Path.h"

#include <cstdlib>

using namespace mlir;

namespace {

/// Assembles the clang command line for one input, selecting the C or C++
/// frontend from `isCxx` (W2.0 per-input language selection): plain C
/// stays `-std=c11` (historical, unchanged); C++ opens with `-x c++
/// -std=c++17`, entirely DROPPING `-std=c11` (clang hard-errors on
/// `-std=c11 -x c++`, which is exactly why this used to reject every
/// `.cpp` input outright). Both add clang's builtin `-resource-dir`
/// (needed for system headers such as `<stdint.h>`) taken from the
/// `EMITRUST_RESOURCE_DIR` environment variable or, failing that, the
/// compile-time `EMITRUST_CLANG_RESOURCE_DIR` macro when defined, and
/// finally the caller's extra arguments in order.
std::vector<std::string>
buildCommandLine(bool isCxx, llvm::ArrayRef<std::string> extraClangArgs) {
  // C89-era programs (the c-testsuite corpus, e.g. 00144's
  // `q = i ? 0 : 0`) assign integer expressions to pointers, which clang
  // >= 15 hard-errors by default; demote it back to the historical
  // warning — the importer itself classifies integer-to-pointer traffic
  // and rejects the unsupported shapes with located diagnostics. The same
  // demotion applies to the C++ frontend for symmetry (the importer's own
  // classification is language-agnostic).
  std::vector<std::string> commandLine =
      isCxx ? std::vector<std::string>{"-x", "c++", "-std=c++17",
                                       "-Wno-error=int-conversion"}
            : std::vector<std::string>{"-std=c11", "-Wno-error=int-conversion"};
  std::string resourceDir;
  if (const char *env = std::getenv("EMITRUST_RESOURCE_DIR"))
    resourceDir = env;
#ifdef EMITRUST_CLANG_RESOURCE_DIR
  if (resourceDir.empty())
    resourceDir = EMITRUST_CLANG_RESOURCE_DIR;
#endif
  if (!resourceDir.empty())
    commandLine.push_back("-resource-dir=" + resourceDir);
  commandLine.insert(commandLine.end(), extraClangArgs.begin(),
                     extraClangArgs.end());
  return commandLine;
}

/// A `CompilationDatabase` that selects the C or C++ command line
/// (`buildCommandLine`) per file by extension (`isCxxSourcePath`),
/// delegating to one `FixedCompilationDatabase` per language (their
/// well-tested `CompileCommand` construction is reused verbatim — only
/// which instance answers a given file differs). This is what lets
/// `importCProject`'s multi-file `ClangTool` compile each input in its
/// own language mode: a mixed C+C++ project is out of scope for W2.0 (no
/// cross-language linkage), but nothing stops each file from compiling
/// correctly in isolation.
class PerFileCompilationDatabase : public clang::tooling::CompilationDatabase {
public:
  explicit PerFileCompilationDatabase(
      llvm::ArrayRef<std::string> extraClangArgs)
      : cDatabase(".", buildCommandLine(/*isCxx=*/false, extraClangArgs)),
        cxxDatabase(".", buildCommandLine(/*isCxx=*/true, extraClangArgs)) {}

  std::vector<clang::tooling::CompileCommand>
  getCompileCommands(llvm::StringRef filePath) const override {
    return (mlir::emitrust::isCxxSourcePath(filePath) ? cxxDatabase : cDatabase)
        .getCompileCommands(filePath);
  }

private:
  clang::tooling::FixedCompilationDatabase cDatabase;
  clang::tooling::FixedCompilationDatabase cxxDatabase;
};

} // namespace

bool mlir::emitrust::isCxxSourcePath(llvm::StringRef path) {
  llvm::StringRef ext = llvm::sys::path::extension(path);
  return ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".C" ||
         ext == ".c++" || ext == ".hpp";
}

int mlir::emitrust::buildProjectASTs(
    llvm::ArrayRef<std::string> paths,
    llvm::ArrayRef<std::string> extraClangArgs,
    std::vector<std::unique_ptr<clang::ASTUnit>> &asts) {
  PerFileCompilationDatabase compilations(extraClangArgs);
  std::vector<std::string> sources(paths.begin(), paths.end());
  clang::tooling::ClangTool tool(compilations, sources);
  return tool.buildASTs(asts);
}

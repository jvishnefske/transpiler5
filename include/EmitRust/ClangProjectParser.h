//===- ClangProjectParser.h - clang parse shell for a project ---*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// The imperative shell that turns a list of source paths into a list of
/// clang `ASTUnit`s: the per-input C/C++ command-line selection, the
/// `-resource-dir` discovery, and the `ClangTool::buildASTs` call.
///
/// This was `importCProject`'s private preamble until FR-40 gave it a
/// second caller (the project item graph, `EmitRust/Project/ItemGraph.h`),
/// which must parse exactly the way the importer does — same `-std`, same
/// per-extension language selection, same resource dir — or the two would
/// disagree about what is even in the project. Sharing the code is the only
/// way to keep that guarantee; re-deriving the command line in the graph
/// would silently diverge the moment either side gains a flag.
///
/// The factoring is deliberately mechanical: `buildProjectASTs` returns
/// `ClangTool::buildASTs`' raw status and leaves every interpretation of it
/// to the caller, because `importC` and `importCProject` classify a
/// partially-built AST list slightly differently (a distinct diagnostic for
/// the single-file case) and neither's behavior may change.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CLANGPROJECTPARSER_H
#define EMITRUST_CLANGPROJECTPARSER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <memory>
#include <string>
#include <vector>

namespace clang {
class ASTUnit;
} // namespace clang

namespace mlir {
namespace emitrust {

/// True when `path`'s extension marks it as a C++ source: `.cpp`, `.cc`,
/// `.cxx`, `.C`, `.c++`, or `.hpp` (W2.0 per-input language selection).
/// Anything else (including a bare `.c` or no extension) stays C.
bool isCxxSourcePath(llvm::StringRef path);

/// Parses each path in `paths` as an INDEPENDENT translation unit and
/// appends the resulting units to `asts`, in the order of `paths`.
///
/// The language is selected per input by extension (`isCxxSourcePath`): C
/// inputs compile as `-std=c11`, C++ inputs as `-x c++ -std=c++17`. Both
/// get clang's builtin `-resource-dir` (from the `EMITRUST_RESOURCE_DIR`
/// environment variable, else the compile-time `EMITRUST_CLANG_RESOURCE_DIR`
/// when defined) and then `extraClangArgs` verbatim, in order. Parse
/// diagnostics are printed to stderr by clang's own machinery.
///
/// The caller owns the interpretation of the outcome: a short `asts`, a
/// null entry, a nonzero status, and `ASTUnit::getDiagnostics()` are all
/// left for it to check, because different entry points report them
/// differently.
///
/// \param paths the source files to parse, in project order.
/// \param extraClangArgs additional clang arguments applied to every input.
/// \param asts receives one unit per successfully parsed input.
/// \returns `clang::tooling::ClangTool::buildASTs`' status: zero when every
///          input compiled without an error.
int buildProjectASTs(llvm::ArrayRef<std::string> paths,
                     llvm::ArrayRef<std::string> extraClangArgs,
                     std::vector<std::unique_ptr<clang::ASTUnit>> &asts);

/// `buildProjectASTs` driven by a `compile_commands.json` (FR-45).
///
/// With an empty `compilationDatabasePath` this is exactly the overload
/// above — the per-extension language guess — so the no-`--compdb` behavior
/// is bit-for-bit what it was. Otherwise the database at that path supplies
/// each input's real command line (its `-x`/`-std`, its `-I`s, and the
/// `directory` its relative paths resolve against), and the extension guess
/// survives only as the fallback for a file the database does not mention.
///
/// The two out-parameters exist because a database changes WHICH files are
/// parsed, not just how: when `paths` is empty and a database was given,
/// the project IS the database, so the resolved translation-unit list is
/// only known here. `resolvedSources` receives it — sorted and deduplicated,
/// because `getAllFiles` iterates a hash map and TU order is observable in
/// the imported module (the `tu<N>_` file-static prefix), and because a
/// database may carry several entries for one file.
///
/// Errors are returned, not diagnosed: this shell has no `MLIRContext` and
/// its two callers report failures differently (the importer as a located
/// MLIR diagnostic, the item-graph tool on stderr). A nonempty `error` and
/// a nonzero return always accompany each other.
///
/// \param paths the source files to parse; empty means "the whole database".
/// \param extraClangArgs arguments appended LAST, so `--extra-arg` overrides
///        the database.
/// \param compilationDatabasePath a directory holding a
///        `compile_commands.json`, the JSON file itself, or empty for none.
/// \param asts receives one unit per successfully parsed input.
/// \param resolvedSources receives the translation-unit list actually used.
/// \param error receives a human-readable reason when the database cannot be
///        loaded; untouched otherwise.
/// \returns zero when every input compiled without an error.
int buildProjectASTs(llvm::ArrayRef<std::string> paths,
                     llvm::ArrayRef<std::string> extraClangArgs,
                     llvm::StringRef compilationDatabasePath,
                     std::vector<std::unique_ptr<clang::ASTUnit>> &asts,
                     std::vector<std::string> &resolvedSources,
                     std::string &error);

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CLANGPROJECTPARSER_H

//===- SectionTable.cpp - section-registered test entry points ------------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Project/SectionTable.h"

#include "EmitRust/CSymbolNaming.h"
#include "EmitRust/ClangProjectParser.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTUnit.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Twine.h"

using namespace mlir;
using namespace mlir::emitrust;

namespace {
/// Every function whose address the walked expression mentions.
///
/// A RECURSIVE walk over `DeclRefExpr`s, not a shape match on the
/// initializer's elements: systemd stores the address as
/// `(union f) &(func)`, so the reference sits under a `CStyleCastExpr
/// <ToUnion>` and a `UnaryOperator '&'`, while the plain `{ func, "name" }`
/// shape puts it under a `FunctionToPointerDecay` directly in the list. A
/// shape-matched element walk finds the second and silently misses the
/// first, which is the whole of the real corpus.
///
/// The same DECL may be reported several times — clang traverses an
/// `InitListExpr` in both its syntactic and its semantic form — so the
/// caller deduplicates.
struct FunctionAddressCollector
    : clang::RecursiveASTVisitor<FunctionAddressCollector> {
  llvm::SmallVector<const clang::FunctionDecl *> functions;

  bool VisitDeclRefExpr(clang::DeclRefExpr *ref) {
    if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()))
      functions.push_back(fn);
    return true;
  }
};
} // namespace

llvm::SmallVector<std::string>
mlir::emitrust::collectSectionTestEntries(llvm::ArrayRef<clang::ASTUnit *> units,
                                          llvm::StringRef section,
                                          unsigned &matchedObjects) {
  llvm::SmallVector<std::string> entries;
  llvm::StringSet<> seen;
  for (auto [index, unit] : llvm::enumerate(units)) {
    if (!unit)
      continue;
    // The per-TU tag of internal-linkage symbols, built exactly as
    // `importCProject` and `buildItemGraph` build it: an entry's registered
    // function is almost always `static`, so this tag is what makes the
    // derived name the emitted one.
    std::string tuTag = ("tu" + llvm::Twine(index) + "_").str();
    for (clang::Decl *decl :
         unit->getASTContext().getTranslationUnitDecl()->decls()) {
      auto *var = llvm::dyn_cast<clang::VarDecl>(decl);
      if (!var || !var->hasInit())
        continue;
      const auto *attr = var->getAttr<clang::SectionAttr>();
      if (!attr || attr->getName() != section)
        continue;
      ++matchedObjects;
      FunctionAddressCollector collector;
      collector.TraverseStmt(const_cast<clang::Expr *>(var->getInit()));
      for (const clang::FunctionDecl *fn : collector.functions) {
        // The SHARED naming, never a re-derivation: FR-53's idiomatic
        // rename and FR-73's underscore fold both live here, and a
        // hand-rolled `tuTag + spelling` was measured to produce
        // `tu0_CamelCase` for a module that defines `tu0_camel_case`.
        std::string symbol = cFunctionSymbolName(fn, tuTag);
        // A non-identifier name outside the admitted operator table emits
        // no symbol at all; nothing to wrap.
        if (symbol.empty())
          continue;
        if (seen.insert(symbol).second)
          entries.push_back(symbol);
      }
    }
  }
  return entries;
}

FailureOr<llvm::SmallVector<std::string>>
mlir::emitrust::collectSectionTestEntries(
    llvm::ArrayRef<std::string> paths,
    llvm::ArrayRef<std::string> extraClangArgs,
    llvm::StringRef compilationDatabasePath, llvm::StringRef section,
    unsigned &matchedObjects, std::string &error) {
  std::vector<std::unique_ptr<clang::ASTUnit>> owned;
  std::vector<std::string> resolvedSources;
  ProjectParseError firstClangError;
  int status = buildProjectASTs(paths, extraClangArgs, compilationDatabasePath,
                                owned, resolvedSources, error, firstClangError);
  if (status != 0) {
    if (error.empty()) {
      error = firstClangError.message.empty()
                  ? std::string("the sources did not parse")
                  : (firstClangError.file.empty()
                         ? firstClangError.message
                         : firstClangError.file + ": " +
                               firstClangError.message);
    }
    return failure();
  }
  llvm::SmallVector<clang::ASTUnit *> units;
  for (const std::unique_ptr<clang::ASTUnit> &unit : owned) {
    if (!unit) {
      error = "a translation unit failed to parse";
      return failure();
    }
    units.push_back(unit.get());
  }
  return collectSectionTestEntries(units, section, matchedObjects);
}

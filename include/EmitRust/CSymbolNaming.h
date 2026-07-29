//===- CSymbolNaming.h - C/C++ decl to emitted symbol naming ----*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// The pure, state-free half of "what Rust item name does this clang
/// declaration become?".
///
/// Why this is a shared header rather than private importer detail (FR-40):
/// two independent consumers must agree, byte for byte, on the emitted
/// symbol of a declaration — the importer (`CImporter::mlirFuncName`,
/// `CImporter::globalVarSymbolName`, `CImporter::structSymbolName`), which
/// creates the MLIR/Rust items, and the project item graph
/// (`EmitRust/Project/ItemGraph.h`), whose node keys must BE those item
/// names so that a graph node and an emitted Rust item are the same thing
/// by construction. Duplicating the mangling in the graph would let the two
/// drift silently, which is exactly the failure mode a whole-project index
/// must not have; so the primitives live here once and both sides call
/// them. `CImporter::mlirFuncName`/`globalVarSymbolName` are thin wrappers
/// that supply their current per-TU tag to `cFunctionSymbolName` /
/// `cGlobalSymbolName` below.
///
/// Rejected alternative: exposing `CImporter` itself (or a naming subobject
/// of it) to the item graph. That would drag the whole IR-building state —
/// an `OpBuilder`, a live `ModuleOp`, the per-TU dedup maps — into what is
/// meant to be a pure AST analysis, and would force the graph to be built
/// during an import rather than standing on its own. The split taken here
/// is the natural one: everything that is a pure function of the clang AST
/// lives here and is shared; everything that depends on accumulated import
/// state (the tag-versus-ordinary-identifier collision renaming in
/// `structSymbolName`, the shape-keyed `Anon<n>` naming, block-scope record
/// mangling) stays in `CImporter` and is deliberately NOT modelled by the
/// graph — see `ItemGraph.h` for what that costs.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CSYMBOLNAMING_H
#define EMITRUST_CSYMBOLNAMING_H

#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

#include <string>

namespace mlir {
namespace emitrust {

/// Returns whether `name` is a Rust keyword (strict or reserved, editions
/// 2015-2021, plus the contextual `union`) and thus unusable as a Rust item
/// name. Global variables keep their C spelling verbatim, so colliding
/// names are rejected instead of being mangled.
static inline bool isRustKeyword(llvm::StringRef name) {
  static const llvm::StringSet<> keywords = {
      // Strict keywords (2015).
      "as", "break", "const", "continue", "crate", "else", "enum", "extern",
      "false", "fn", "for", "if", "impl", "in", "let", "loop", "match", "mod",
      "move", "mut", "pub", "ref", "return", "self", "Self", "static",
      "struct", "super", "trait", "true", "type", "unsafe", "use", "where",
      "while",
      // Strict keywords (2018).
      "async", "await", "dyn",
      // Reserved keywords.
      "abstract", "become", "box", "do", "final", "macro", "override", "priv",
      "try", "typeof", "unsized", "virtual", "yield",
      // Contextual keyword that still reads confusingly as an item name.
      "union"};
  return keywords.contains(name);
}

/// Returns the Rust spelling of a struct/union MEMBER name: a spelling that
/// is a Rust keyword mangles deterministically by appending a single
/// underscore (`type` -> `type_`, `match` -> `match_`); every other
/// spelling is kept verbatim. Member names are the one identifier class
/// that mangles instead of rejecting (C99-45: c-testsuite 00218 declares a
/// member named `type`): the mangled spelling never leaves the emitted
/// struct's own field namespace, so no cross-symbol collision policy is
/// disturbed. Struct/enum/function/global names keep their rejections. A
/// collision the mangle introduces (a struct declaring both `type` and
/// `type_`) is rejected where the fields are collected.
static inline std::string mangleMemberName(llvm::StringRef name) {
  if (isRustKeyword(name))
    return (name + "_").str();
  return name.str();
}

/// Returns the C-declared Rust-facing name of a record: its tag name, or,
/// for a tagless record declared through `typedef struct { ... } T;`, the
/// typedef name. Returns an empty StringRef for a bare anonymous struct,
/// for which `importRecord` synthesizes a shape-keyed `Anon<n>` name
/// (retrieved through `CImporter::emittedRecordName`). The typedef name is
/// the record's name for all mangling and cross-TU shape-dedup purposes,
/// exactly like a tagged struct. This is the base spelling only: for
/// file-scope records `CImporter::structSymbolName` layers the tag-versus-
/// ordinary-namespace collision renaming on top, and block-scope records
/// take the `<function>_<tag>` mangle in `importRecord`.
static inline llvm::StringRef recordRustName(const clang::RecordDecl *record) {
  llvm::StringRef name = record->getName();
  if (!name.empty())
    return name;
  if (const clang::TypedefNameDecl *typedefName =
          record->getTypedefNameForAnonDecl())
    return typedefName->getName();
  return {};
}

/// W2.0 C++ input tolerance: the `ns_<name>_`-per-level prefix reflecting
/// `context`'s enclosing namespace chain, applied to a declaration's
/// emitted module symbol name (`mlirFuncName`, the global-variable naming
/// in `importGlobalVar`/`collectOrdinaryNames`) so `ns::foo` never
/// collides with a top-level `foo`. Built outermost-to-innermost; nested
/// namespaces compose (`a::b::foo` -> `ns_a_ns_b_foo`). An anonymous
/// namespace — one synthesized internal-linkage entity per translation
/// unit, however many `namespace { ... }` blocks reopen it — contributes
/// the fixed tag `ns_anon_`. `extern "C" { ... }` (`LinkageSpecDecl`) is
/// transparent: it is not a `NamespaceDecl`, so it is skipped while
/// walking up and contributes nothing, matching C linkage's unchanged-name
/// contract. Returns the empty string for plain C input, where no
/// `NamespaceDecl` ever appears in a `DeclContext` chain.
static inline std::string namespacePrefix(const clang::DeclContext *context) {
  llvm::SmallVector<const clang::NamespaceDecl *, 4> chain;
  for (; context && !context->isTranslationUnit();
       context = context->getParent())
    if (const auto *ns = llvm::dyn_cast<clang::NamespaceDecl>(context))
      chain.push_back(ns);
  std::string prefix;
  for (const clang::NamespaceDecl *ns : llvm::reverse(chain)) {
    prefix += "ns_";
    prefix += ns->isAnonymousNamespace() ? "anon" : ns->getName().str();
    prefix += "_";
  }
  return prefix;
}

/// The emitted module symbol of the function `func` in a translation unit
/// whose per-TU mangling tag is `tuTag` (`"tu<i>_"` under a multi-file
/// import, empty for a single-file one).
///
/// The rules, in the order they compose:
///  - `main` becomes `c_main`, unconditionally and with no prefixing: the
///    emitted crate's real `fn main` is a wrapper the crate emitter writes.
///  - a C spelling that is a Rust keyword mangles like a struct member —
///    one trailing underscore (`match` -> `match_`, CTS 00204) — rather
///    than being rejected. The mangled spelling is the symbol's identity
///    everywhere (definition and call sites resolve through this same
///    function); a collision with an existing `match_` is rejected in
///    `CImporter::importFunction`.
///  - a C++ namespace chain contributes its flattening prefix ahead of the
///    mangled base name (`namespacePrefix`, empty for plain C input, where
///    a `FunctionDecl`'s `DeclContext` is never a `NamespaceDecl`);
///    `extern "C"` contributes nothing, preserving C linkage's
///    unchanged-name contract.
///  - an internal-linkage (`static`) function takes `tuTag` in front, so
///    identically named file-statics in different TUs never collide.
///
/// \param func the function declaration to name.
/// \param tuTag the per-TU tag for internal-linkage symbols; empty for a
///        single-translation-unit import, preserving the historical bare name.
/// \returns the emitted module symbol name.
static inline std::string cFunctionSymbolName(const clang::FunctionDecl *func,
                                              llvm::StringRef tuTag) {
  llvm::StringRef cName = func->getName();
  if (cName == "main")
    return "c_main";
  std::string base =
      namespacePrefix(func->getDeclContext()) + mangleMemberName(cName);
  if (func->getStorageClass() == clang::SC_Static)
    return (tuTag + base).str();
  return base;
}

/// The emitted module symbol of the file-scope variable `var` in a
/// translation unit whose per-TU mangling tag is `tuTag`.
///
/// Unlike function names, a global's C spelling is never mangled (a global
/// named like a Rust keyword is rejected at import instead). An
/// internal-linkage global — `static` at file scope, or any global in an
/// anonymous namespace — takes `tuTag` in front so identically named
/// file-statics in different TUs stay distinct; a C++ namespace chain
/// contributes its flattening prefix (`namespacePrefix`).
///
/// \param var the file-scope variable declaration to name.
/// \param tuTag the per-TU tag for internal-linkage symbols; empty for a
///        single-translation-unit import.
/// \returns the emitted module symbol name.
static inline std::string cGlobalSymbolName(const clang::VarDecl *var,
                                            llvm::StringRef tuTag) {
  bool internal = !var->isExternallyVisible();
  return (internal ? tuTag.str() : std::string()) +
         namespacePrefix(var->getDeclContext()) + var->getName().str();
}

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CSYMBOLNAMING_H

//===- CSymbolNaming.h - C/C++ decl to emitted symbol naming ----*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// The shared half of "what Rust item name does this clang declaration
/// become?". Every primitive here is a pure function of the clang AST plus
/// ONE piece of ambient state: the process-wide `idiomaticRenameEnabled()`
/// flag (FR-53), set once at driver startup, which selects between verbatim
/// C spellings and the idiomatic Rust rename. See the flag's own comment for
/// why a single global beats threading a parameter through both consumers.
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

#include "EmitRust/CSymbolLinkage.h"
#include "EmitRust/RustCasing.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

#include <string>

namespace mlir {
namespace emitrust {

/// FR-53 idiomatic rename. Process-wide because the SAME naming primitives feed
/// two independent driver paths that must agree byte-for-byte (see the file
/// header): the importer that creates MLIR/Rust items, and the FR-40 item graph
/// that runs its own clang parse with no importer in scope. A single source of
/// truth makes drift between the two impossible; a per-call parameter threaded
/// through both paths could silently diverge on a missed site. It is set once,
/// at startup, by the driver (`emitrust-cc`; default = rename ON, disabled by
/// `--preserve-c-names`). Tools that do not set it (e.g. `emitrust-import-c`)
/// keep verbatim C spellings, so their golden tests are unaffected.
inline bool &idiomaticRenameEnabled() {
  static bool enabled = false;
  return enabled;
}

// The casing primitives (`toSnakeCase`, `toScreamingSnakeCase`,
// `toUpperCamelCase`) live in EmitRust/RustCasing.h, included above: they are
// clang-free and the FR-70 lowering pass in lib/Conversion (MLIR-only) must
// share the exact derivation. Everything below is a function of the clang AST.

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
  std::string base =
      idiomaticRenameEnabled() ? toSnakeCase(name) : name.str();
  if (isRustKeyword(base))
    return base + "_";
  return base;
}

/// The Rust spelling of a fully-assembled function symbol (base name plus any
/// structural prefixes/suffixes): `snake_case` under the idiomatic rename,
/// verbatim otherwise.
static inline std::string fnRustName(llvm::StringRef name) {
  return idiomaticRenameEnabled() ? toSnakeCase(name) : name.str();
}

/// The Rust spelling of a fully-assembled type symbol (struct/enum, including
/// synthesized `Owner_<fn>_<base>` / block-scope `<fn>_<tag>` names):
/// `UpperCamelCase` under the idiomatic rename, verbatim otherwise.
static inline std::string typeRustName(llvm::StringRef name) {
  return idiomaticRenameEnabled() ? toUpperCamelCase(name) : name.str();
}

/// The Rust spelling of a fully-assembled global/static symbol (module-level
/// state, including synthesized `<name>_backing` storage and `<fn>_<name>`
/// static-local mangles): `SCREAMING_SNAKE_CASE` under the idiomatic rename,
/// verbatim otherwise.
static inline std::string globalRustName(llvm::StringRef name) {
  return idiomaticRenameEnabled() ? toScreamingSnakeCase(name) : name.str();
}

/// The Rust name of an enum type: `UpperCamelCase` under the idiomatic rename,
/// the verbatim C tag otherwise. Applied identically at the enum definition,
/// every enum value use, and the item-graph node so the three stay consistent.
static inline std::string enumTypeRustName(llvm::StringRef name) {
  return typeRustName(name);
}

/// The Rust name of an enum variant's associated constant:
/// `SCREAMING_SNAKE_CASE` under the idiomatic rename, verbatim otherwise.
static inline std::string enumVariantRustName(llvm::StringRef name) {
  return globalRustName(name);
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
static inline std::string recordRustName(const clang::RecordDecl *record) {
  llvm::StringRef name = record->getName();
  if (name.empty())
    if (const clang::TypedefNameDecl *typedefName =
            record->getTypedefNameForAnonDecl())
      name = typedefName->getName();
  if (name.empty())
    return {};
  return idiomaticRenameEnabled() ? toUpperCamelCase(name) : name.str();
}

/// FR-73: joins a structural symbol prefix (the per-TU statics tag
/// `tu<i>_`, the namespace chain `ns_a_`) onto a mangled base name without
/// manufacturing consecutive underscores. Every non-empty prefix ends in
/// `_`, and `toSnakeCase` never doubles an existing underscore, so prefix
/// concatenation was the ONE producer of the `__` that rustc's denied
/// non_snake_case lint rejects outright (`tu0__set` fails the emitted
/// crate's build; rustc's own suggestion is the fold applied here). The
/// rule: ALL leading underscores of the base collapse into the boundary
/// underscore the prefix already carries (`tu0_` + `_set` -> `tu0_set`,
/// `tu0_` + `__x` -> `tu0_x`, `ns_a_` + `_f` -> `ns_a_f`); with an empty
/// prefix the base is untouched (a bare leading underscore is legal
/// snake_case). A fold that would merge two distinct C spellings (`_set`
/// and `set` in one TU both composing to `tu0_set`) is rejected with a
/// located diagnostic where the later declaration is imported
/// (`CImporter::importFunction` / `importGlobalVar`, backed by the
/// pre-scan's composed-name-to-raw-spelling map) — never silently unified.
static inline std::string joinSymbolPrefix(llvm::StringRef prefix,
                                           llvm::StringRef base) {
  if (prefix.empty())
    return base.str();
  return (prefix + base.ltrim('_')).str();
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

/// W2.15 template-argument TYPE CODE: the deterministic spelling one
/// `Type` template argument contributes to a monomorphized function
/// template's emitted symbol.
///
/// This is a table SEPARATE from W2.2's overload codes
/// (`cxxOverloadParamCode` in lib/ImportC/ImportCFunctions.cpp), and
/// deliberately so: that table spells EVERY integer `i`, a choice pinned
/// byte-for-byte by test/Import/Cpp/methods.cpp (`Counter_get_i`) and
/// harmless there because one class rarely overloads on `int` versus
/// `long`. Two instantiations of ONE template, by contrast, are two
/// different functions with the same C++ spelling and the same source
/// location, so a code that fuses `add<int>` with `add<long>` makes the
/// second definition collide with the first. The template table is
/// therefore WIDER: integers carry width and signedness.
///
/// Every code is snake-safe BY CONSTRUCTION — the record/enum code runs
/// through `toSnakeCase` unconditionally (not only under the FR-53
/// idiomatic rename), because the suffix is appended AFTER
/// `mangleMemberName` has done its own casing and would otherwise escape
/// it: a `struct P` argument emitting `sum_P` is not merely ugly, it is a
/// hard `cargo build` failure under rustc's denied `non_snake_case` lint.
///
/// The `x` fallback cannot disambiguate two arguments that both land on
/// it (two distinct function-pointer types, two distinct lambda closure
/// types). That is not a silent miscompile: the second instantiation
/// collides on the composed symbol and `CImporter::importFunction`
/// rejects it with a located, template-aware wording (pinned in
/// test/Import/Cpp/function-templates-invalid.cpp). Widening the table is
/// how a future wave admits such a shape.
static inline std::string templateArgTypeCode(clang::QualType type) {
  clang::QualType canonical = type.getCanonicalType().getUnqualifiedType();
  if (canonical->isVoidType())
    return "v";
  // Checked before the builtin switch: `bool` is a builtin AND satisfies
  // `isIntegerType`, and it must not share an integer code.
  if (canonical->isBooleanType())
    return "b";
  // An enum is an integer type but has its OWN identity: two distinct
  // enums instantiating one template are two different functions, so they
  // take their tag names rather than a shared integer code.
  if (const auto *enumType = canonical->getAs<clang::EnumType>()) {
    if (const clang::IdentifierInfo *id = enumType->getDecl()->getIdentifier())
      return toSnakeCase(id->getName());
    return "x";
  }
  if (const auto *builtin = canonical->getAs<clang::BuiltinType>()) {
    switch (builtin->getKind()) {
    case clang::BuiltinType::Char_S:
    case clang::BuiltinType::SChar:
      return "i8";
    case clang::BuiltinType::Char_U:
    case clang::BuiltinType::UChar:
    case clang::BuiltinType::Char8:
      return "u8";
    case clang::BuiltinType::Short:
      return "i16";
    case clang::BuiltinType::UShort:
    case clang::BuiltinType::Char16:
      return "u16";
    case clang::BuiltinType::Int:
    case clang::BuiltinType::WChar_S:
      return "i32";
    case clang::BuiltinType::UInt:
    case clang::BuiltinType::WChar_U:
    case clang::BuiltinType::Char32:
      return "u32";
    case clang::BuiltinType::Long:
    case clang::BuiltinType::LongLong:
      return "i64";
    case clang::BuiltinType::ULong:
    case clang::BuiltinType::ULongLong:
      return "u64";
    case clang::BuiltinType::Float:
      return "f";
    case clang::BuiltinType::Double:
    case clang::BuiltinType::LongDouble:
      return "d";
    default:
      return "x";
    }
  }
  // A pointer NESTS its pointee's code behind `p`, so `int *` and
  // `double *` stay distinct arguments.
  if (canonical->isPointerType())
    return "p" + templateArgTypeCode(canonical->getPointeeType());
  if (const auto *recordType = canonical->getAs<clang::RecordType>()) {
    if (const clang::IdentifierInfo *id = recordType->getDecl()->getIdentifier())
      return toSnakeCase(id->getName());
    // An unnamed record — most importantly a lambda closure type — has no
    // stable spelling to code with.
    return "x";
  }
  return "x";
}

/// W2.15 template-argument suffix: `""` for an ordinary function, and
/// `"_" + code` per template argument, in template-parameter declaration
/// order, for a function-template specialization.
///
/// It lives here, inside the shared naming header, for one load-bearing
/// reason: EVERY name-recomputation site in the project (call sites,
/// `resolveFunctionPointerDecl`, the Pass-A planners, recovery, and the
/// FR-40 item graph) reaches a function's symbol through
/// `cFunctionSymbolName`. Appending the suffix in the importer's
/// definition path instead would leave all of them computing the
/// UNSUFFIXED name, and `add<int>(2, 3)` would resolve to nothing
/// ("call to unimported function 'add'"). Putting it here is also what
/// makes design.md's "a call site needs no separate handling" true, and
/// keeps the CLAUDE.md byte-identity invariant (importer and item graph
/// agree on a symbol) intact for free.
///
/// A NON-type argument codes as the `x` fallback rather than asserting:
/// this must stay a total function of the AST for the item graph's sake.
/// The importer never emits such a symbol — non-type arguments and
/// parameter packs are LOCATED rejections in
/// `CImporter::importTopLevelDecl` before any naming happens.
static inline std::string
templateArgSuffix(const clang::FunctionDecl *func) {
  const clang::TemplateArgumentList *args =
      func->getTemplateSpecializationArgs();
  if (!args)
    return std::string();
  std::string suffix;
  for (const clang::TemplateArgument &arg : args->asArray()) {
    suffix += "_";
    suffix += arg.getKind() == clang::TemplateArgument::Type
                  ? templateArgTypeCode(arg.getAsType())
                  : std::string("x");
  }
  return suffix;
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
///  - a W2.15 function-template specialization takes one `_<code>` suffix
///    per template argument (`templateArgSuffix`, above), which is what
///    keeps `add<int>` and `add<long>` distinct symbols.
///  - an internal-linkage (`static`) function takes `tuTag` in front, so
///    identically named file-statics in different TUs never collide.
///  - each prefix joins through `joinSymbolPrefix` (FR-73): leading
///    underscores of the prefixed name fold into the boundary so the
///    composition never manufactures the `__` rustc's denied
///    non_snake_case lint rejects.
///
/// FR-51 reads that last rule BACKWARDS at Rust-emission time to decide
/// which items a library crate exports; the inverse predicate is
/// `isInternalLinkageSymbolName` in `EmitRust/CSymbolLinkage.h`, which must
/// be kept in step with the tagging here.
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
  std::string base = joinSymbolPrefix(namespacePrefix(func->getDeclContext()),
                                      mangleMemberName(cName));
  //  - W2.15: a function-template specialization appends one type code per
  //    template argument (`templateArgSuffix`), so two instantiations of one
  //    template never share a symbol. The suffix goes on AFTER the namespace
  //    prefix and BEFORE the internal-linkage tag join, so a `static`
  //    template in a namespace composes all three.
  base += templateArgSuffix(func);
  if (func->getStorageClass() == clang::SC_Static)
    return joinSymbolPrefix(tuTag, base);
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
/// contributes its flattening prefix (`namespacePrefix`). The prefixes
/// join through `joinSymbolPrefix` (FR-73): a leading-underscore C
/// spelling folds into the prefix boundary (`_x` -> `tu0_x`) instead of
/// composing the `tu0__x` that rustc's denied non_snake_case lint (or its
/// SCREAMING_SNAKE_CASE sibling for the renamed `TU0__X`) rejects.
///
/// \param var the file-scope variable declaration to name.
/// \param tuTag the per-TU tag for internal-linkage symbols; empty for a
///        single-translation-unit import.
/// \returns the emitted module symbol name.
static inline std::string cGlobalSymbolName(const clang::VarDecl *var,
                                            llvm::StringRef tuTag) {
  bool internal = !var->isExternallyVisible();
  std::string full =
      joinSymbolPrefix((internal ? tuTag.str() : std::string()) +
                           namespacePrefix(var->getDeclContext()),
                       var->getName());
  // A global becomes SCREAMING_SNAKE_CASE as a whole, so its per-TU tag and
  // namespace prefix are uppercased too (`tu0_calls` -> `TU0_CALLS`,
  // `ns_shapes_base` -> `NS_SHAPES_BASE`); the linkage predicate in
  // CSymbolLinkage.h recognizes the uppercased tags.
  return idiomaticRenameEnabled() ? toScreamingSnakeCase(full) : full;
}

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CSYMBOLNAMING_H

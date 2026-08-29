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
#include "clang/AST/DeclFriend.h"
#include "clang/AST/DeclTemplate.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

#include <string>

namespace mlir {
namespace emitrust {

// The FR-53 idiomatic-rename flag (`idiomaticRenameEnabled()`), the casing
// primitives (`toSnakeCase`, `toScreamingSnakeCase`, `toUpperCamelCase`) and
// the `non_snake_case` lint predicate (`tripsNonSnakeCase`) live in
// EmitRust/RustCasing.h, included above: they are clang-free, and consumers
// that must share the EXACT derivation without clang in scope -- the FR-70
// lowering pass in lib/Conversion, the FR-140 Rust emitter in lib/Target --
// would otherwise have to duplicate them and could then silently drift.
// Everything below is a function of the clang AST.

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
/// W2.16: forward declaration only — the definition sits beside its
/// `FunctionDecl` sibling below, next to the `templateArgTypeCode` table
/// both overloads share, while its one caller (`recordRustName`) has to
/// come first because the rest of this header's type-naming layer is
/// built on it.
static inline std::string templateArgSuffix(const clang::RecordDecl *record);

/// FR-108: forward declarations only — both are DEFINED below (beside the
/// function-symbol layer that has always used them), but `recordRustName`
/// has to come first because the rest of this header's type-naming layer
/// is built on it.
static inline std::string joinSymbolPrefix(llvm::StringRef prefix,
                                           llvm::StringRef base);
static inline std::string namespacePrefix(const clang::DeclContext *context);

static inline std::string recordRustName(const clang::RecordDecl *record) {
  llvm::StringRef name = record->getName();
  if (name.empty())
    if (const clang::TypedefNameDecl *typedefName =
            record->getTypedefNameForAnonDecl())
      name = typedefName->getName();
  if (name.empty())
    return {};
  // W2.16: a class-template specialization takes one `_<code>` suffix per
  // template argument, and it goes on BEFORE the idiomatic rename — the
  // OPPOSITE placement from W2.15's function side, where the suffix is
  // appended to an already-snake_cased base. The ordering is load-bearing,
  // not cosmetic: appended AFTER the rename, `Box<int>` would emit as
  // `Box_i32`, which rustc's denied `non_camel_case_types` lint rejects
  // outright; folded in first it becomes `BoxI32`, which is lint-clean and
  // still injective per code. Everything downstream is free — the
  // per-class method mangle reads the assigned struct name out of
  // `CImporter::assignedStructNames`, so `Box_i32_get` -> `box_i32_get`
  // falls out with no edit.
  //
  // FR-108: the record name takes the SAME `ns_<name>_`-per-level
  // namespace prefix `cFunctionSymbolName` has always applied. Without it
  // `::Box` and `ns::Box` composed one spelling, the second definition
  // was silently merged into the first by the shape-keyed dedup in
  // `importRecordUncached`, and every `ns::Box` call site dispatched to
  // `::Box`'s method bodies (measured: native `1 101`, emitted crate
  // `1 1`). The prefix goes on BEFORE the idiomatic camel fold, exactly
  // like the template suffix and for the same reason: folded in first,
  // `ns::Box<int>` becomes the lint-clean `NsNsBoxI32` rather than an
  // `ns_ns_Box_i32` that rustc's denied `non_camel_case_types` refuses.
  // Being a pure function of the AST, it composes for free at the three
  // sites that recompute a record symbol without importer state
  // (`ItemGraphBuilder::recordSymbolFor`, FR-41's coloring probe, FR-42's
  // recovery owner symbol). `std::` records never reach here — their
  // names are pre-seeded — so `std::pair<int, int>` stays `PairI32I32`.
  std::string spelled =
      joinSymbolPrefix(namespacePrefix(record->getDeclContext()),
                       name.str() + templateArgSuffix(record));
  return idiomaticRenameEnabled() ? toUpperCamelCase(spelled) : spelled;
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
///
/// FR-125: under the idiomatic rename each SEGMENT folds to snake_case
/// (`namespace Game` -> `ns_game_`), exactly as `mangleMemberName` folds
/// the base name -- a verbatim CamelCase segment fails the emitted
/// crate's denied `non_snake_case` lint (`ns_Game_score` was exit-0
/// unbuildable; measured 33 errors across 9 corpus crates). The fold is
/// per-segment inside the loop so nested chains (`ns_game_ns_input_`)
/// and the tuTag prefix compose unchanged, and it is GATED on
/// `idiomaticRenameEnabled()` so `--preserve-c-names` and
/// `emitrust-import-c` keep the verbatim spelling byte-for-byte. Two
/// namespaces distinguished only by case (`Game::f`, `game::f`) now fold
/// onto one symbol; the qualified-owner guard in `importFunction`
/// rejects that located, never merging (see `ordinaryTuQualifiedOwners`).
static inline std::string namespacePrefix(const clang::DeclContext *context) {
  llvm::SmallVector<const clang::NamespaceDecl *, 4> chain;
  for (; context && !context->isTranslationUnit();
       context = context->getParent())
    if (const auto *ns = llvm::dyn_cast<clang::NamespaceDecl>(context))
      chain.push_back(ns);
  std::string prefix;
  for (const clang::NamespaceDecl *ns : llvm::reverse(chain)) {
    prefix += "ns_";
    if (ns->isAnonymousNamespace())
      prefix += "anon";
    else
      prefix += idiomaticRenameEnabled() ? toSnakeCase(ns->getName())
                                         : ns->getName().str();
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

/// W2.28 non-type (integral) template-argument code: `v` + the decimal
/// value, with `n` for a negative sign (`Fixed<3>` -> `fixed_v3` ->
/// `FixedV3`, `addN<-3>` -> `add_n_vn3`). Bool, char and enum non-type
/// arguments are `TemplateArgument::Integral` too and code by VALUE the
/// same way — two distinct enumerators of one enum are two distinct
/// instantiations, and the numeric value is the only spelling that is
/// total (an enumerator has a name; `Fixed<3 + 4>` does not).
///
/// The `v` prefix is NOT decoration — it is what keeps the code one word
/// under EVERY casing. A bare-digit code is snake-safe but dies under the
/// FR-53 UpperCamelCase record rename, which erases underscores between
/// chunks: `Grid<1,23>` and `Grid<12,3>` both camel to `Grid123`, and the
/// probe measured that fusing two same-shaped specializations SILENTLY
/// dispatches both to whichever method body was imported first (the
/// shape-conflict guard only catches the differently-shaped case). With
/// the prefix they are `GridV1V23` and `GridV12V3`. Pinned in
/// test/EndToEnd/cpp-template-nttp.cpp.
static inline std::string
templateArgIntegralCode(const llvm::APSInt &value) {
  llvm::SmallString<32> digits;
  value.toString(digits, /*Radix=*/10);
  std::string code(digits.begin(), digits.end());
  if (!code.empty() && code.front() == '-')
    code = "n" + code.substr(1);
  return "v" + code;
}

/// W2.15/W2.28 shared per-argument code: type arguments through the
/// W2.15 type table, integral non-type arguments through the W2.28 value
/// code, everything else (declaration, nullptr, template-template,
/// packs) the `x` placeholder. Total by construction for the item
/// graph's sake; the importer gates (`checkTemplateArguments`,
/// `checkClassTemplateSpecialization`) reject every `x`-coded kind with
/// a located diagnostic before any such symbol is emitted.
static inline std::string
templateArgCode(const clang::TemplateArgument &arg) {
  if (arg.getKind() == clang::TemplateArgument::Type)
    return templateArgTypeCode(arg.getAsType());
  if (arg.getKind() == clang::TemplateArgument::Integral)
    return templateArgIntegralCode(arg.getAsIntegral());
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
/// An INTEGRAL non-type argument codes by value (W2.28); every other
/// non-type kind codes as the `x` fallback rather than asserting: this
/// must stay a total function of the AST for the item graph's sake. The
/// importer never emits an `x`-coded symbol — those kinds and parameter
/// packs are LOCATED rejections in `CImporter::importTopLevelDecl`
/// before any naming happens.
static inline std::string
templateArgSuffix(const clang::FunctionDecl *func) {
  const clang::TemplateArgumentList *args =
      func->getTemplateSpecializationArgs();
  if (!args)
    return std::string();
  std::string suffix;
  for (const clang::TemplateArgument &arg : args->asArray()) {
    suffix += "_";
    suffix += templateArgCode(arg);
  }
  return suffix;
}

/// W2.16 template-argument suffix for a RECORD: `""` for an ordinary
/// struct/class, and `"_" + code` per template argument, in
/// template-parameter declaration order, for a class-template
/// specialization. It reuses `templateArgTypeCode` VERBATIM rather than
/// minting a second table, so `Box<int>` and `add<int>` code the same
/// argument the same way.
///
/// It lives here, inside `recordRustName`'s reach, for the same
/// load-bearing reason the function overload lives inside
/// `cFunctionSymbolName`: all 8 name-RECOMPUTATION sites in the project
/// (`structSymbolName`, the rejected-record wording,
/// `importRecordUncached`, `emittedRecordName`, the two FR-42 recovery
/// sites, the FR-40 item graph's `recordSymbolFor`, the coloring probe)
/// reach a record's symbol through `recordRustName`. Suffixing at the
/// struct_def emission site alone would leave every one of them naming an
/// UNSUFFIXED type, and a local of type `Box<int>` would resolve to
/// nothing.
///
/// An INTEGRAL non-type argument codes by value (W2.28); every other
/// non-type kind codes as the `x` fallback rather than asserting: this
/// must stay a total function of the AST for the item graph's sake. The
/// importer never emits an `x`-coded symbol — those kinds and parameter
/// packs are LOCATED rejections in `CImporter::importRecordUncached`
/// before any struct_def is created.
static inline std::string templateArgSuffix(const clang::RecordDecl *record) {
  const auto *spec =
      llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record);
  if (!spec)
    return std::string();
  std::string suffix;
  for (const clang::TemplateArgument &arg : spec->getTemplateArgs().asArray()) {
    suffix += "_";
    suffix += templateArgCode(arg);
  }
  return suffix;
}

/// FR-114 per-parameter type code for a FREE-function overload suffix:
/// `templateArgTypeCode`'s table verbatim, plus an `r`-prefixed arm for
/// REFERENCE parameters (`const S &` -> `rs`), which the template table
/// never needed — a template argument deduced through a reference
/// parameter is the referenced type itself. Reusing the W2.15 table
/// keeps one type coding one way everywhere (`g(double)` -> `g_d`,
/// `add<double>` -> `add_d`); the `r` prefix keeps `f(S)` and
/// `f(const S&)` distinct symbols, which is what resolves W2.23's noted
/// `T(const T&)`/`T(const U&)` hazard once copy constructors are
/// admitted. Codes that the table cannot split (long/long long both
/// `i64`, double/long double both `d`, two function pointers both `px`)
/// are NOT silent: the composed symbols collide and
/// `CImporter::importFunction` rejects the second with the FR-114
/// overload-set wording (pinned in
/// test/Import/Cpp/overload-collisions-invalid.cpp).
static inline std::string overloadArgTypeCode(clang::QualType type) {
  clang::QualType canonical = type.getCanonicalType();
  if (canonical->isReferenceType())
    return "r" + templateArgTypeCode(canonical.getNonReferenceType());
  return templateArgTypeCode(type);
}

/// W2.25: the synthesized identifier base name of an ADMITTED overloaded
/// operator kind, or the empty StringRef for every kind outside the wave's
/// subset. The table is the single source of truth for which operator
/// KINDS may take a symbol at all — the importer's def gates, the member
/// omission screen, the FR-40 item graph and the FR-41 coloring probe all
/// consult it through `operatorSymbolBaseName` below, so admission can
/// never diverge between them. Deliberately absent, each with its own
/// fence elsewhere: `operator=` (W2.23's implicit-copy-assign special case
/// interplay), `++`/`--` (the canonical `S r = *this` body has no image),
/// `<<`/`>>` (stream territory), compound assignments and `->`/`&`/`,`
/// (unspiked). A kind outside this table keeps its historical located
/// rejection ("unsupported: overloaded operator" at the def, "omitted from
/// class" at a member call).
static inline llvm::StringRef
overloadedOperatorSymbolBaseName(clang::OverloadedOperatorKind kind) {
  switch (kind) {
  case clang::OO_EqualEqual:
    return "op_eq";
  case clang::OO_ExclaimEqual:
    return "op_ne";
  case clang::OO_Less:
    return "op_lt";
  case clang::OO_LessEqual:
    return "op_le";
  case clang::OO_Greater:
    return "op_gt";
  case clang::OO_GreaterEqual:
    return "op_ge";
  case clang::OO_Plus:
    return "op_add";
  case clang::OO_Minus:
    return "op_sub";
  case clang::OO_Star:
    return "op_mul";
  case clang::OO_Slash:
    return "op_div";
  case clang::OO_Percent:
    return "op_rem";
  case clang::OO_Exclaim:
    return "op_not";
  case clang::OO_Call:
    return "op_call";
  case clang::OO_Subscript:
    return "op_index";
  default:
    return llvm::StringRef();
  }
}

/// W2.25: the synthesized base name `func` emits under, when `func` is an
/// overloaded operator of an admitted kind — empty for every other
/// DeclarationName shape. Literal operators (`operator""_kb`) are a
/// different DeclarationName kind and return empty; an operator TEMPLATE
/// (pattern or specialization) is excluded the way `overloadParamSuffix`
/// excludes specializations — the W2.15 suffix rules were never spiked
/// against synthesized spellings, so templates keep FR-119's rejection.
static inline std::string
operatorSymbolBaseName(const clang::FunctionDecl *func) {
  if (func->getDeclName().getNameKind() !=
      clang::DeclarationName::CXXOperatorName)
    return std::string();
  if (func->getDescribedFunctionTemplate() ||
      func->getTemplateSpecializationArgs())
    return std::string();
  return overloadedOperatorSymbolBaseName(func->getOverloadedOperator()).str();
}

/// FR-114: the size of the same-TU overload set `func` belongs to — the
/// number of same-named, non-template FunctionDecls its (transparent-
/// context-skipping) declaration context declares. Redeclarations
/// COLLAPSE: a DeclContext's lookup table stores one entry per entity,
/// so a plain-C prototype + definition pair counts as 1 and C input can
/// never grow a suffix. A FunctionTemplateDecl is not a FunctionDecl and
/// so never counts (its instantiations are suffixed by
/// `templateArgSuffix` instead); the belt-and-braces
/// `getDescribedFunctionTemplate` filter keeps any templated-pattern
/// FunctionDecl a lookup might surface from counting either. Methods and
/// constructors never reach this predicate through `cFunctionSymbolName`
/// (they are named by `CImporter::cxxMethodMangledName`), but
/// `importFunction`'s collision wording shares this free-function half of
/// the overload-set test.
static inline unsigned overloadSetSize(const clang::FunctionDecl *func) {
  // W2.25 lifted the blanket non-identifier exclusion here: an ADMITTED
  // free operator counts its same-DeclarationName siblings exactly like an
  // identifier function (three free `operator*` overloads are the measured
  // FR-119 raytracing shape, and without the count they all map to one
  // `op_mul`). A user function literally named `op_eq` is a DIFFERENT
  // DeclarationName and never joins the set — the cross-spelling collision
  // rejects located through the FR-125 qualified-owner guard instead.
  if (!func->getDeclName().isIdentifier() &&
      operatorSymbolBaseName(func).empty())
    return 1;
  const clang::DeclContext *context =
      func->getDeclContext()->getRedeclContext();
  unsigned size = 0;
  for (const clang::NamedDecl *sibling : context->lookup(func->getDeclName()))
    if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(sibling))
      if (!fn->getDescribedFunctionTemplate())
        ++size;
  return size;
}

/// FR-114 free-function overload suffix: `""` for a function that is the
/// SOLE non-template owner of its name (every pre-FR-114 import — the
/// zero-churn guarantee), and one `_<code>` per parameter, in declaration
/// order, when the declaration context holds a genuine C++ overload set.
/// A zero-parameter overload contributes no `_<code>` at all (the loop
/// never runs), so it keeps the bare historical name — a zero-arg C++
/// signature is unique within its overload set by construction
/// (raytracing's `random_double()` vs `random_double(double, double)` is
/// the worked example, pinned in test/Import/Cpp/overload-suffix.cpp).
///
/// It lives here, inside the shared naming header, for W2.15's
/// load-bearing reason: EVERY name-recomputation site in the project
/// (call sites, `resolveFunctionPointerDecl`, the Pass-A planners,
/// recovery, the FR-40 item graph, the FR-41 coloring probe) reaches a
/// free function's symbol through `cFunctionSymbolName`, so all of them
/// compute the identical suffix BY CONSTRUCTION. Suffixing in the
/// importer's definition path instead would leave the item graph and the
/// --incremental probe on the UNSUFFIXED name — which is precisely the
/// pre-FR-114 defect, where the collision cascaded into a spurious
/// "call argument type mismatch" that stubbed c_main itself.
///
/// Guards, each deliberate:
///  - a template specialization is excluded (its `templateArgSuffix`
///    already separates instantiations; the pattern's name is claimed by
///    the hand-written/instantiation collision rules, W2.15);
///  - a non-identifier name is excluded through `overloadSetSize` UNLESS
///    it is a W2.25-admitted free operator (`operatorSymbolBaseName`
///    non-empty), which suffixes exactly like an identifier overload set;
///    every other shape (literal operators, non-admitted kinds) stays
///    FR-119 territory and is rejected before naming matters;
///  - a method never arrives here (see `overloadSetSize`), so the member
///    scheme's frozen W2.2 codes are untouched.
static inline std::string
overloadParamSuffix(const clang::FunctionDecl *func) {
  if (func->getTemplateSpecializationArgs())
    return std::string();
  if (llvm::isa<clang::CXXMethodDecl>(func))
    return std::string();
  if (overloadSetSize(func) <= 1)
    return std::string();
  std::string suffix;
  for (const clang::ParmVarDecl *param : func->parameters()) {
    suffix += "_";
    suffix += overloadArgTypeCode(param->getType());
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
  // W2.25: an admitted overloaded operator has no identifier spelling at
  // all — its base name is SYNTHESIZED from the operator kind (`operator==`
  // -> `op_eq`) and then composes with the very same namespace prefix,
  // FR-114 overload suffix and linkage tag an identifier function takes. A
  // non-identifier name OUTSIDE the admitted table returns the EMPTY string
  // (never asserting in `getName()`), which every consumer treats as "this
  // declaration emits no symbol": the importer rejects it located (FR-119)
  // and the FR-40 graph/coloring skip it.
  std::string mangledBase;
  if (func->getDeclName().isIdentifier()) {
    llvm::StringRef cName = func->getName();
    if (cName == "main")
      return "c_main";
    mangledBase = mangleMemberName(cName);
  } else {
    mangledBase = operatorSymbolBaseName(func);
    if (mangledBase.empty())
      return std::string();
  }
  std::string base = joinSymbolPrefix(namespacePrefix(func->getDeclContext()),
                                      mangledBase);
  //  - FR-114: a member of a free-function OVERLOAD SET appends one type
  //    code per parameter (`overloadParamSuffix`, empty for every sole
  //    owner of a name), so `g(int)`/`g(double)` emit `g_i32`/`g_d`
  //    instead of colliding. It composes before the template suffix
  //    textually, but the two are mutually exclusive by construction (a
  //    specialization returns the empty overload suffix).
  base += overloadParamSuffix(func);
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

/// FR-123: the friend functions DEFINED INLINE in the class definition
/// `record`, in declaration order — the item-scope declarations that the
/// walks over a `TranslationUnitDecl`'s `decls()` structurally cannot see.
///
/// A friend function's `FunctionDecl` hangs off a `FriendDecl` inside the
/// `CXXRecordDecl`; its SEMANTIC declaration context is the enclosing
/// namespace (so it names, and is named, exactly like a free function),
/// but it is reachable only through the record's LEXICAL member list. So
/// every walk that must agree on the program's item set — the importer's
/// `importDeclsIn` and `collectOrdinaryNamesFrom`, the item graph's three
/// passes, the FR-41 admissibility probe — has to ask for these
/// explicitly. It lives HERE, beside `cFunctionSymbolName`, for the same
/// reason the naming primitives do: five walks agreeing by construction
/// rather than by discipline. A walk that admitted a different friend set
/// than the importer would put items in the graph the crate never emits,
/// or (the direction FR-123 exists to close) leave an emitted function out
/// of the denominator and report 1000 permille for incomplete output.
///
/// The selection rule, clause by clause:
///  * DEFINITION-CARRYING ONLY. A body-less friend PROTOTYPE is skipped:
///    its out-of-class definition is already a top-level declaration every
///    walk reaches, so admitting the prototype here would double-import
///    it. (An unreferenced body-less prototype with no definition anywhere
///    keeps skipping silently, which is `importFunction`'s deliberate
///    referenced-only policy.)
///  * NON-MEMBER ONLY. `friend void Other::f();` names another class's
///    method; it carries no body at the friend site, and members are
///    screened out explicitly besides — a method's symbol needs its
///    class's assigned struct name, which this header deliberately does
///    not model.
///  * NON-TEMPLATE ONLY. A friend function template's `getFriendDecl` is a
///    `FunctionTemplateDecl` (not a `FunctionDecl`) and is skipped by the
///    cast; an explicit/implicit specialization is skipped by the
///    specialization-kind clause. RECORDED GAP, not silence: a use still
///    fails loudly at its call site with "call to unimported function".
///    The same holds for a hidden friend of a CLASS template, which is
///    reached only through the class-template arm's `specializations()`
///    and is deliberately not walked — one hidden-friend symbol per
///    instantiation carries no suffix distinguishing the instantiations,
///    so `Box<int>` and `Box<long>` would both claim `op_add`.
///  * A friend TYPE declaration (`friend class W;`) has no `FunctionDecl`
///    at all and contributes nothing: it grants access and declares no
///    code.
///
/// Nested classes are not recursed into: a nested record is itself a loud
/// rejection at import ("unsupported: struct definition outside file or
/// function scope"), so no friend hides silently behind one.
///
/// \param record the class definition to scan; a non-definition (a forward
///        declaration) contributes nothing, which is what keeps a record
///        declared twice at file scope from yielding its friends twice.
/// \param out receives the selected friend definitions, appended.
static inline void collectFriendDefinitions(
    const clang::CXXRecordDecl *record,
    llvm::SmallVectorImpl<const clang::FunctionDecl *> &out) {
  if (!record || !record->isCompleteDefinition())
    return;
  for (const clang::Decl *member : record->decls()) {
    if (member->isImplicit())
      continue;
    const auto *friendDecl = llvm::dyn_cast<clang::FriendDecl>(member);
    if (!friendDecl)
      continue;
    const clang::NamedDecl *named = friendDecl->getFriendDecl();
    if (!named)
      continue; // A friend TYPE: `getFriendType` is the populated half.
    const auto *func = llvm::dyn_cast<clang::FunctionDecl>(named);
    if (!func || llvm::isa<clang::CXXMethodDecl>(func))
      continue;
    if (!func->doesThisDeclarationHaveABody())
      continue;
    if (func->getDescribedFunctionTemplate() ||
        func->getTemplateSpecializationKind() != clang::TSK_Undeclared)
      continue;
    out.push_back(func);
  }
}

/// The `collectFriendDefinitions` overload for a walk that holds a plain
/// `clang::Decl *`: yields nothing unless `decl` is a class DEFINITION.
static inline void collectFriendDefinitions(
    const clang::Decl *decl,
    llvm::SmallVectorImpl<const clang::FunctionDecl *> &out) {
  collectFriendDefinitions(llvm::dyn_cast<clang::CXXRecordDecl>(decl), out);
}

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CSYMBOLNAMING_H

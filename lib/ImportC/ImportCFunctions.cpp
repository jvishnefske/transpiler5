//===- ImportCFunctions.cpp - function import ------------------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// CImporter's function import: mlirFuncName/importFunction, the
/// bindOrdinaryParam/bindCursorParam parameter-binding family,
/// emitCursorWritebacks, emitVaClones/emitVaClone/emitVaArg,
/// importTranslationUnit/importDeclsIn (the latter is the recursive
/// per-decl dispatch shared with `namespace`/`extern "C"` bodies, W2.0),
/// and the low-level function-body-plumbing helpers
/// (createEntryAlloca/createBlock/getLabelBlock/createVariablePlace/
/// isTerminated/createIntConstant/createBoolConstant/
/// createScalarIntConstant/finalizeFunction). Split out of ImportC.cpp by
/// pure code motion (W1.9); see CImporterInternal.h for the CImporter class
/// declaration this file implements. The Pass-A planners embedded between
/// emitVaArg and importTranslationUnit in the original file
/// (planCursorParams/planVaMonomorph) are left behind in ImportC.cpp per the
/// plan (a later multi-TU wave restructures the Pass-A planners together).
//
//===----------------------------------------------------------------------===//

#include "CImporterInternal.h"

using namespace mlir;

std::string CImporter::mlirFuncName(const clang::FunctionDecl *func) const {
  // The whole rule (`main` -> `c_main`, the Rust-keyword member mangle, the
  // W2.0 namespace flattening prefix, the per-TU tag on internal-linkage
  // names) lives in the shared `cFunctionSymbolName`; the only thing this
  // method adds is the importer's current per-TU tag. It is shared rather
  // than private so the FR-40 item graph keys its function nodes on the
  // very same spelling — see EmitRust/CSymbolNaming.h.
  return cFunctionSymbolName(func, currentTuTag);
}

/// W2.2: `method`'s un-suffixed mangled base name — the fixed spelling
/// "new" for a constructor (whose `DeclarationName` is the special
/// `CXXConstructorName` kind and has no ordinary identifier), or its C++
/// spelling passed through the same keyword-escape a struct field name
/// uses (`mangleMemberName`).
static std::string cxxMethodBaseName(const clang::CXXMethodDecl *method) {
  if (llvm::isa<clang::CXXConstructorDecl>(method))
    return "new";
  // W2.17: a destructor's `DeclarationName` is the special
  // `CXXDestructorName` kind and has no ordinary identifier, so `getName()`
  // would assert. The fixed base name is `dtor`, deliberately NOT `drop`:
  // a member function literally spelled `void drop()` is legal C++ and
  // already takes the module symbol `<Struct>_drop` (measured), so reusing
  // it would silently merge two different functions. The in-IMPL symbol is
  // separately renamed to `drop` by `convert-func-to-emitrust`.
  if (llvm::isa<clang::CXXDestructorDecl>(method))
    return "dtor";
  // FR-117: every REMAINING `DeclarationName` kind a `CXXMethodDecl` can
  // carry that is not an ordinary identifier -- `CXXConversionFunctionName`
  // (`operator int()`) and `CXXOperatorName` (`operator+`) -- has NO
  // spelling to mangle. Falling through to `getName()` here was a live
  // defect: it asserts in clang/AST/Decl.h (`Name is not a simple
  // identifier`), and with that assert compiled out by our release NDEBUG
  // build it returned the EMPTY STRING, so the class emitted a method
  // literally named `<Struct>_` -- a symbol nobody can call, and one that
  // two conversion functions in the same class collided on outright. The
  // empty return is the OMISSION marker: `importCXXMethods` skips such a
  // method, `cxxMethodMangledName`'s overload counter ignores it, and
  // `importFunction` refuses it outright, so it can never reach a symbol.
  if (!method->getDeclName().isIdentifier())
    return std::string();
  return mangleMemberName(method->getName());
}

/// W2.2 per-overload parameter type code, widened by FR-114. The two W2.2
/// codes are FROZEN byte-for-byte by CHECK pins (`i` for any non-bool
/// integer — test/Import/Cpp/methods.cpp's Counter_new_i/Counter_get_i,
/// cpp-defaulted-ctor.cpp's C_new_i — and `b` for bool), so they keep
/// their historical spellings and their historical breadth: int/long and
/// int/unsigned still share `i` on the member path even though the free
/// path splits them (_i32/_i64). Everything that previously fell to the
/// `x` placeholder now delegates to W2.15's `templateArgTypeCode` table
/// (d/f/p<pointee>/record-snake-name), with two FR-114 additions layered
/// on top:
///  - a REFERENCE parameter takes the `r` prefix over its referenced
///    type's code (`const S &` -> `rs`, via `overloadArgTypeCode`), which
///    is what splits the motivating `S(double,double)` /
///    `S(const S&,const S&)` pair into S_new_dd / S_new_rsrs;
///  - an ENUM is carved out ahead of the frozen `i` arm (an enum
///    satisfies `isIntegerType`, so it used to collide with a genuine int
///    overload) and takes its tag-name code, the same choice W2.15 made.
/// Codes concatenate WITHOUT separators on the member path (frozen by the
/// same pins), so multi-char record codes can alias across overloads
/// (`h(A, Bi)` and `h(Ab, I)` both compose `abi`). That is not silent:
/// the composed symbols collide and `importFunction` rejects the second
/// with the FR-114 overload-set wording — the documented backstop, pinned
/// in test/Import/Cpp/overload-collisions-invalid.cpp.
static std::string cxxOverloadParamCode(clang::QualType type) {
  clang::QualType canonical = type.getCanonicalType();
  if (canonical->isReferenceType())
    return emitrust::overloadArgTypeCode(canonical);
  canonical = canonical.getUnqualifiedType();
  if (canonical->isBooleanType())
    return "b";
  if (canonical->isEnumeralType())
    return emitrust::templateArgTypeCode(canonical);
  if (canonical->isIntegerType())
    return "i";
  return emitrust::templateArgTypeCode(canonical);
}

/// FR-114: is `func` a member of a genuine same-TU C++ overload set (>1
/// same-named, non-implicit, non-template function in its own declaration
/// context)? This is the discriminator between the honest overload-set
/// collision wording and the genuine cross-TU duplicate wording in
/// `importFunction`: a lookup of size 1 means the colliding earlier
/// definition can only have come from ANOTHER translation unit. Methods
/// and constructors count their siblings on the class (the same walk
/// `cxxMethodMangledName`'s suffix decision uses — `getDeclName()`
/// equality covers named methods and the shared `CXXConstructorName`
/// alike); free functions share `overloadSetSize`'s lookup-based count,
/// where redeclarations collapse.
static bool isSameTUOverloadSet(const clang::FunctionDecl *func) {
  if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(func)) {
    unsigned sharing = 0;
    for (const clang::CXXMethodDecl *candidate :
         method->getParent()->methods()) {
      if (candidate->isImplicit() || candidate->isDeleted())
        continue;
      if (candidate->getDeclName() == method->getDeclName())
        ++sharing;
    }
    return sharing > 1;
  }
  return emitrust::overloadSetSize(func) > 1;
}

std::string
CImporter::cxxMethodMangledName(const clang::CXXMethodDecl *method) const {
  const clang::CXXRecordDecl *record = method->getParent();
  // `lookup` returns the mapped std::string BY VALUE; binding it to a
  // StringRef would dangle the moment the temporary dies (observed as
  // garbage bytes in mangled names under the dylib build).
  std::string structName = assignedStructNames.lookup(record);
  std::string baseName = cxxMethodBaseName(method);
  // The overload suffix is present only when the class declares MORE THAN
  // ONE method (or constructor) sharing this base name (a genuine C++
  // overload set) — counted fresh here rather than cached, since it is a
  // pure function of the (already-fully-declared, by the time any method
  // imports) class AST.
  unsigned sharingCount = 0;
  for (const clang::CXXMethodDecl *candidate : record->methods()) {
    if (candidate->isImplicit() || candidate->isDeleted() ||
        llvm::isa<clang::CXXDestructorDecl>(candidate))
      continue;
    // FR-117: an OMITTED member (empty base name) is not part of any
    // overload set — it has no symbol at all — so it must not perturb the
    // suffixing of the siblings that do. This loop is the reason FR-117 had
    // to land before any wave that admits more member shapes: it walks
    // EVERY method of the class, so one omitted member is enough to drag
    // all of them through this path.
    std::string candidateName = cxxMethodBaseName(candidate);
    if (candidateName.empty())
      continue;
    if (candidateName == baseName)
      ++sharingCount;
  }
  std::string mangled = structName + "_" + baseName;
  if (sharingCount > 1) {
    std::string codes;
    for (const clang::ParmVarDecl *param : method->parameters())
      codes += cxxOverloadParamCode(param->getType());
    // A zero-parameter overload contributes an empty code string, which
    // keeps the bare `<StructName>_<methodBaseName>` spelling with no
    // trailing suffix (methods.cpp's binding worked example: `Counter_get`
    // for the 0-arg overload, `Counter_get_i` for the 1-arg one).
    if (!codes.empty())
      mangled += ("_" + codes);
  }
  // The `<Class>_<method>[_codes]` symbol is a free-standing function name, so
  // it takes the function spelling (snake_case under the idiomatic rename,
  // folding the UpperCamel class prefix back to snake).
  return fnRustName(mangled);
}

/// Returns whether `later` differs from `earlier` only by refining
/// `!emitrust.mut_ref<T>` parameter positions into
/// `!emitrust.mut_ref<!emitrust.slice<T>>` — the shape change a
/// definition's pointer-parameter classification may introduce over a
/// prototype-only import from another translation unit.
static bool isSliceRefinementOf(FunctionType earlier, FunctionType later) {
  if (earlier.getNumInputs() != later.getNumInputs() ||
      earlier.getResults() != later.getResults())
    return false;
  for (auto [oldType, newType] :
       llvm::zip_equal(earlier.getInputs(), later.getInputs())) {
    if (oldType == newType)
      continue;
    auto oldRef = llvm::dyn_cast<emitrust::MutRefType>(oldType);
    auto newRef = llvm::dyn_cast<emitrust::MutRefType>(newType);
    if (!oldRef || !newRef)
      return false;
    auto newSlice = llvm::dyn_cast<emitrust::SliceType>(newRef.getPointee());
    if (!newSlice || newSlice.getElementType() != oldRef.getPointee())
      return false;
  }
  return true;
}

LogicalResult CImporter::importFunction(const clang::FunctionDecl *func,
                                        bool signatureOnly) {
  Location loc = translateLoc(func->getLocation());
  // FR-117 corrects what this comment used to claim. A constructor's
  // `DeclarationName` has no ordinary identifier spelling
  // (`CXXConstructorName` is a distinct `DeclarationName::NameKind`), and it
  // is NOT the only one: the old text asserted that "every OTHER
  // `CXXMethodDecl` this function ever sees has an ordinary identifier",
  // which was measurably false for DESTRUCTORS (13 hits on the shipping
  // test/EndToEnd/cpp-destructor.cpp alone), for conversion functions, for a
  // UNION's overloaded operators (the union import path never runs the W2.2
  // member-shape gate), and for a NON-MEMBER `operator+`, which is an
  // ordinary top-level `FunctionDecl` and never went near a record gate at
  // all. `getName()` asserts on every one of them
  // (clang/AST/Decl.h: "Name is not a simple identifier"); our release
  // NDEBUG build compiles that assert out and returns "", so instead of
  // failing they produced unnamed symbols -- the free operator emitted
  // `emitrust.func @<<INVALID EMPTY SYMBOL>>` and the literally unparseable
  // Rust `pub fn (v0: A, b: i32) -> i32`, a crate that cannot build, with no
  // diagnostic anywhere.
  //
  // So the identifier test is asked ONCE, of the `DeclarationName` itself,
  // and `cName` is left EMPTY rather than asking `getName()`, for every one
  // of them.
  //
  // The located refusal below is scoped to `CXXMethodDecl`s, which is
  // FR-117's subject: it is the backstop for the omission
  // `importCXXMethods` performs, so that a conversion function reached any
  // OTHER way (an out-of-line definition, which is a top-level item in its
  // own right) fails loudly instead of emitting an unnamed symbol. A
  // NON-MEMBER operator does NOT reject here: FR-119 closes that channel
  // with its own guard placed AFTER the referenced-only prototype skip
  // further down. Placement is load-bearing -- widening THIS guard to
  // `!namedByIdentifier` alone was measured to break two pins, because an
  // unreferenced body-less operator PROTOTYPE (stl-map-invalid.cpp's free
  // `operator<`) must keep skipping silently, and the deliberate cost is
  // exactly one wording (ostream-invalid.cpp's user `operator<<` now says
  // `unsupported: overloaded operator` instead of the STL-type tail's
  // vaguer fence). See the FR-119 guard below.
  const auto *cxxMethod = llvm::dyn_cast<clang::CXXMethodDecl>(func);
  bool cxxIsCtor =
      cxxMethod && llvm::isa<clang::CXXConstructorDecl>(cxxMethod);
  const bool cxxIsDtor =
      cxxMethod && llvm::isa<clang::CXXDestructorDecl>(cxxMethod);
  const bool namedByIdentifier = func->getDeclName().isIdentifier();
  if (cxxMethod && !namedByIdentifier && !cxxIsCtor && !cxxIsDtor)
    return emitError(loc)
           << (llvm::isa<clang::CXXConversionDecl>(func)
                   ? "unsupported: conversion function"
                   : "unsupported: overloaded operator");
  llvm::StringRef cName =
      namedByIdentifier ? func->getName() : llvm::StringRef();

  // Recovery stub retry (FR-42): a variadic function has no Rust signature
  // at all — its named parameters do not describe its call sites — so there
  // is nothing to stub and the item stays dropped. Checked before the
  // variadic handling below so the retry never re-enters `emitVaClones`,
  // whose per-clone import is exactly the body import the stub replaces.
  if (recoveryStubOnly && func->isVariadic())
    return failure();

  if (func->isVariadic()) {
    const clang::FunctionDecl *definition = func->getDefinition();
    if (definition && definition->hasBody()) {
      // A variadic definition whose body never touches va_list (no
      // va_start/va_arg/va_copy, no va_list declarations) can never
      // observe its trailing arguments, so it imports as its fixed
      // prototype — the named parameters only (CTS-P9). Call sites drop
      // effect-free trailing extras in `emitCall`. A body that uses
      // va_list in the bounded shape monomorphizes per call site
      // (CTS 00204, planVaMonomorph); any other va_list-using body
      // keeps the blanket rejection.
      if (bodyUsesVaList(astContext(), definition->getBody())) {
        auto planIt = vaMonomorphPlans.find(definition->getCanonicalDecl());
        if (planIt == vaMonomorphPlans.end())
          return emitError(loc)
                 << "unsupported: variadic function definition";
        if (!func->isThisDeclarationADefinition())
          return success(); // Clones are emitted at the definition.
        return emitVaClones(definition, planIt->second);
      }
    } else {
      // Body-less variadic declarations (printf in particular) are
      // skipped; calls to them are handled specially or rejected at the
      // call site.
      return success();
    }
  }
  // Body-less puts/putchar declarations are skipped like printf's: their
  // statement-position calls are lowered by name (`emitPuts`/`emitPutchar`)
  // and never reference the symbol, and a body-less function would
  // otherwise be rejected by `finalizeProject`.
  if ((cName == "puts" || cName == "putchar") && !func->getDefinition())
    return success();

  // Referenced-only import of main-file prototypes (the same policy
  // system-header declarations follow, C99-39): a body-less prototype with
  // no definition in this TU that nothing in this TU references demands no
  // definition and imports nothing — not even its signature types. A
  // referenced prototype is still imported, and `finalizeProject` rejects
  // it at the use site if no translation unit supplies the body.
  if (!func->isThisDeclarationADefinition() && !func->getDefinition() &&
      !func->isReferenced())
    return success();

  // FR-119: a free (non-member) operator -- operator+, operator<<, a
  // literal operator""_kb, anything whose DeclarationName is not an
  // ordinary identifier -- is a plain FunctionDecl and slipped past the
  // `cxxMethod &&` refusal above: `cFunctionSymbolName` returned "" (the
  // getName() assert compiles out under NDEBUG), MLIR accepted the empty
  // sym_name, and the crate carried the unparseable `fn (v0: A, b: i32)`
  // with no diagnostic anywhere. Rejected HERE, after the referenced-only
  // skip, so unreferenced body-less operator prototypes stay silently
  // skipped by design; the same wording rides the existing
  // cxx-operator-overload ledger tag, and under --recover the operator
  // drops as its own item. The emitrust.func verifier's empty-sym_name
  // check is the emission-side backstop for whatever this guard cannot
  // see.
  if (!cxxMethod && !namedByIdentifier)
    return emitError(loc) << "unsupported: overloaded operator";

  // FR-47: `signatureOnly` forces the body-less path for a declaration that
  // DOES have a body, so `importCXXMethods`'s prepass can register every
  // method of a class as an external stub before any of their bodies import.
  // Expressed by clearing `isDefinition` alone (rather than by an early
  // return from a separate signature-building routine) so that the stub's
  // signature is computed by the ONE code path below that also computes the
  // definition's — any divergence would trip the redeclaration check in the
  // definition pass instead of silently emitting two shapes.
  bool isDefinition = !signatureOnly && func->isThisDeclarationADefinition();
  // C `main` is renamed so the driver can emit its own Rust `main` wrapper;
  // the replacement name is therefore reserved, and any other spelling that
  // Rust reserves cannot be emitted as a Rust function name.
  if (cName == "c_main")
    return emitError(loc) << "unsupported: function name 'c_main' is "
                             "reserved for the imported C main";
  if (cName == "__emitrust_fmt_f64")
    return emitError(loc) << "unsupported: function name '__emitrust_fmt_f64' "
                             "is reserved for the printf %f helper";
  if (cName == "__emitrust_fmt_c")
    return emitError(loc) << "unsupported: function name '__emitrust_fmt_c' "
                             "is reserved for the printf %c helper";
  if (cName == "__emitrust_cstr")
    return emitError(loc) << "unsupported: function name '__emitrust_cstr' "
                             "is reserved for the printf %s helper";
  if (cName == "__emitrust_strlen")
    return emitError(loc) << "unsupported: function name '__emitrust_strlen' "
                             "is reserved for the strlen helper";
  if (cName.starts_with("__emitrust_"))
    return emitError(loc) << "unsupported: function name '" << cName
                          << "' is in the reserved '__emitrust_' helper "
                             "namespace";
  // A function whose C spelling is a Rust keyword mangles with a trailing
  // underscore (mlirFuncName, CTS 00204) instead of rejecting. The mangle
  // must not silently merge two C symbols: a source declaration already
  // spelled with the mangled name rejects the keyword function where it
  // is declared.
  if (isRustKeyword(cName) &&
      ordinaryRawTuNames.contains((cName + "_").str()))
    return emitError(loc) << "unsupported: function name '" << cName
                          << "' mangles to '" << cName
                          << "_', which collides with an existing symbol";
  // W2.2: a genuine C++ method or constructor takes its own naming path
  // (`cxxMethodMangledName`), decoupled from `mlirFuncName`'s per-TU
  // static-storage-class tag — a C++ method and a C file-static function
  // both report `clang::SC_Static` for unrelated reasons (C++ class-scope
  // linkage vs. C internal linkage), and only the free-function path may
  // pick up that tag.
  std::string name =
      cxxMethod ? cxxMethodMangledName(cxxMethod) : mlirFuncName(func);
  // W2.15: remember that a function-template instantiation claimed this
  // composed spelling, so a LATER hand-written definition colliding with
  // it gets the template-aware wording below rather than the cross-TU one.
  if (func->getTemplateSpecializationArgs())
    templateSpecSymbolNames.insert(name);
  // FR-73: leading underscores of an internal-linkage or namespaced name
  // fold into the prefix boundary (`_set` -> `tu0_set`; `joinSymbolPrefix`
  // in CSymbolNaming.h), and the fold must not silently merge two C
  // symbols: without this guard a prototype-only `static int _set(int)`
  // would be "satisfied" by a same-TU `set` definition and every
  // `_set(...)` call would execute set's body. When a DIFFERENT raw
  // spelling in this TU composes to the same emitted name, the later
  // declaration is rejected where it appears. Scoped to underscore folds
  // (one side's raw spelling leads with '_') so the pre-existing collision
  // machinery keeps its own wordings; C++ methods take their own naming
  // path and are outside the pre-scanned ordinary map.
  if (!cxxMethod) {
    std::string firstRaw = ordinaryTuNameOwners.lookup(name);
    if (!firstRaw.empty() && firstRaw != cName &&
        (cName.starts_with("_") ||
         llvm::StringRef(firstRaw).starts_with("_")))
      return emitError(loc)
             << "unsupported: function name '" << cName << "' emits as '"
             << name << "', which collides with '" << firstRaw
             << "' (leading underscores fold into the symbol prefix)";
    // FR-125: the idiomatic rename folds namespace segments and free-
    // function base names to snake_case, so two DIFFERENT qualified C++
    // spellings can compose to one emitted symbol (`Game::f` and
    // `game::f` both -> `ns_game_f`; `n::myFunc` beside `n::my_func`).
    // The fold must never merge them: the reconciliation's
    // `!isDefinition` early-success cannot tell a redeclaration from a
    // case-fold alias, so a prototype-only `Game::f` would be silently
    // "satisfied" by `game::f`'s body and every call would run the wrong
    // code (measured pre-FR-125 at the member level, exit 0). Keyed on
    // the EMITTED name, so it is mode-sensitive by construction: with
    // the rename off the spellings never collide and both import.
    // Template instantiations are exempt on either side of the clash --
    // their aliasing is the suffix table's, diagnosed with the W2.15
    // wording at reconciliation.
    if (!func->getTemplateSpecializationArgs() &&
        !templateSpecSymbolNames.contains(name)) {
      std::string firstQualified = ordinaryTuQualifiedOwners.lookup(name);
      std::string qualified = func->getQualifiedNameAsString();
      if (!firstQualified.empty() && firstQualified != qualified)
        return emitError(loc)
               << "unsupported: function '" << qualified << "' emits as '"
               << name << "', which collides with '" << firstQualified
               << "' (the idiomatic rename folds both spellings onto one "
                  "symbol)";
    }
  }

  // W2.17: a destructor-carrying class crossing a call boundary BY VALUE is
  // a silent miscompile, not an unmodeled construct. C++ destroys the
  // callee's parameter COPY as well as the caller's object -- the destructor
  // runs TWICE -- while Rust moves and runs it once (measured). The subset
  // models no copy constructor, so nothing could make the counts agree.
  // Checked on the signature, ahead of every parameter-classification path,
  // so a prototype and its definition reject identically. The receiver is
  // not a parameter here (it is a reference, added below) and so is exempt.
  // W2.26: transitive -- a droppy-DERIVED class crossing by value has the
  // same two-runs-vs-one divergence through its inherited destructor.
  for (const clang::ParmVarDecl *param : func->parameters())
    if (userOrInheritedDestructor(astContext(), param->getType()))
      return emitError(translateLoc(param->getLocation()))
             << "unsupported: class with a destructor passed or returned by "
                "value";
  // W2.23 narrowed the RETURN half only: a droppy class WITH an admitted
  // copy constructor may be returned by value -- the return copy lowers
  // into a temp place that is MOVED out (never dropped in the callee), so
  // there is no second destructor run to lose, while the named locals drop
  // in reverse declaration order exactly as C++ destroys them after the
  // return copy (byte-diffed in test/EndToEnd/cpp-copy-dtor.cpp). The
  // PARAMETER half stays wholesale: native destroys the parameter temp at
  // the end of the CALLER's full-expression, the move-into-callee image
  // inside the callee -- measured divergent with two calls in one
  // expression (the spike's twocall probe), so no copy ctor can make the
  // drop POINTS agree.
  if (userOrInheritedDestructor(astContext(), func->getReturnType()) &&
      !admittedCopyConstructor(
          func->getReturnType().getCanonicalType()->getAsCXXRecordDecl()))
    return emitError(loc)
           << "unsupported: class with a destructor passed or returned by "
              "value";

  // K&R callsite-prototype inference (FR-29, CTS 00209): the definition's
  // body refines argument-called prototype-less fn-ptr decls to their
  // callsite signatures BEFORE the signature is built, so a prototype
  // visited ahead of its later definition maps the identical refined
  // parameter types (the reconciliation below sees no conflict). The
  // result is staged locally and installed into `inferredFnPtrSigs` only
  // in the definition's own prologue: a block-scope prototype imported
  // MID-BODY must not clobber the enclosing function's live map.
  const clang::FunctionDecl *definition = func->getDefinition();
  llvm::DenseMap<const clang::VarDecl *, emitrust::FnPtrType> inferredSigs;
  if (definition && definition->doesThisDeclarationHaveABody() &&
      failed(inferNoProtoCallSignatures(definition, inferredSigs)))
    return failure();

  // Build the signature. Pointer-parameter kinds derive from the
  // definition's body (Phase 1b); a prototype whose definition appears
  // later in the same TU classifies identically because
  // `FunctionDecl::getDefinition` searches the whole redeclaration chain.
  // A method-planned function (Phase 4; prototypes consult the same plan,
  // keyed by the canonical declaration) instead trades every data-pointer
  // parameter for an i64 element index behind a leading owner receiver.
  const clang::VarDecl *methodOwner =
      methodPlans.lookup(func->getCanonicalDecl());
  emitrust::StructType ownerStructType;
  ArrayRef<ParamKind> paramKinds = classifyPointerParams(func);
  SmallVector<Type> inputTypes;
  if (methodOwner) {
    ownerStructType = emitrust::StructType::get(
        builder.getContext(), ownerPlans.find(methodOwner)->second.structName);
    inputTypes.push_back(emitrust::MutRefType::get(ownerStructType));
  }
  // W2.2: a genuine C++ instance method (mutually exclusive with
  // `methodOwner`, the unrelated Phase-4 owner-promotion mechanism) takes
  // its receiver as a leading `!emitrust.mut_ref<struct>` (mutating method
  // or constructor) or `!emitrust.ref<struct>` (const method) argument; a
  // static method takes none.
  bool cxxHasReceiver = cxxMethod && !cxxMethod->isStatic();
  emitrust::StructType cxxOwnerStructType;
  if (cxxMethod) {
    // By-value lookup: a StringRef binding would dangle (see
    // cxxMethodMangledName).
    std::string ownerName =
        assignedStructNames.lookup(cxxMethod->getParent());
    // LOAD-BEARING since FR-118, no longer merely defensive: when a class
    // fails a class-level gate the undo in `importRecordUncached` ERASES its
    // `assignedStructNames` entry, so an OUT-OF-LINE definition of one of its
    // methods (a top-level item, not gated by `rejectedRecords`) arrives here
    // with no owner name. Without this check it would mangle `""_get` and
    // emit a method onto a struct that no longer exists. Do not "simplify" it
    // away: test/Import/Cpp/cpp-rejected-class-no-trace.cpp pins it.
    if (ownerName.empty())
      return emitError(loc) << "unsupported: method of an unimported class";
    cxxOwnerStructType =
        emitrust::StructType::get(builder.getContext(), ownerName);
    if (cxxHasReceiver)
      inputTypes.push_back(cxxMethod->isConst()
                                ? Type(emitrust::RefType::get(cxxOwnerStructType))
                                : Type(emitrust::MutRefType::get(
                                      cxxOwnerStructType)));
  }
  if (name == "c_main" && func->getNumParams() != 0) {
    // C `main`'s standard two-parameter form (C99 5.1.2.2.1): `argc`
    // imports as a plain i32 — the crate's `fn main` wrapper passes the
    // process argument count — and `argv`, whose `char **` shape has no
    // safe decomposition, is dropped from the imported signature. A body
    // that reads `argv` is rejected here with a located diagnostic, so
    // the dropped parameter can never be observed.
    if (func->getNumParams() != 2 ||
        !astContext().hasSameUnqualifiedType(
            func->getParamDecl(0)->getType().getCanonicalType(),
            astContext().IntTy))
      return emitError(loc) << "unsupported: main must take zero or two "
                               "(int, char **) parameters";
    const clang::ParmVarDecl *argvParam = func->getParamDecl(1);
    clang::QualType argvType = argvParam->getType().getCanonicalType();
    const auto *outer = argvType->getAs<clang::PointerType>();
    const auto *inner =
        outer ? outer->getPointeeType().getCanonicalType()
                    ->getAs<clang::PointerType>()
              : nullptr;
    if (!inner || !astContext().hasSameUnqualifiedType(
                      inner->getPointeeType(), astContext().CharTy))
      return emitError(loc) << "unsupported: main must take zero or two "
                               "(int, char **) parameters";
    // C99-43 C3: when planning admitted EVERY argv use (whole-value
    // `argv[i]` printf `%s` arguments; `argv[i][j]` byte reads as
    // values), the signature carries the `!emitrust.argv_table` input
    // and the crate wrapper passes the collected `Vec<Vec<i8>>` borrow.
    // The admitted param is recorded on the DEFINITION; a same-TU
    // prototype visit consults it so both build one signature.
    const clang::FunctionDecl *definition = func->getDefinition();
    const clang::ParmVarDecl *defArgvParam =
        definition && definition->getNumParams() == 2
            ? definition->getParamDecl(1)
            : argvParam;
    bool argvAdmitted =
        mainArgvAdmittedParam && defArgvParam == mainArgvAdmittedParam;
    if ((argvParam->isReferenced() || argvParam->isUsed()) && !argvAdmitted)
      return emitError(translateLoc(argvParam->getLocation()))
             << "unsupported: use of main's argv parameter (command-line "
                "argument values are not modeled)";
    inputTypes.push_back(builder.getIntegerType(32));
    if (argvAdmitted)
      inputTypes.push_back(
          emitrust::ArgvTableType::get(builder.getContext()));
  } else {
    for (auto [index, param] : llvm::enumerate(func->parameters())) {
      if (methodOwner && isPointerType(param->getType()) &&
          !isFunctionPointer(param->getType())) {
        inputTypes.push_back(builder.getIntegerType(64));
        continue;
      }
      // A callsite-inferred prototype-less fn-ptr parameter (FR-29, CTS
      // 00209) refines to the inferred signature instead of the
      // zero-parameter no-proto mapping; keyed by the DEFINITION's decl
      // so a prototype visit maps identically.
      if (definition && index < definition->getNumParams())
        if (emitrust::FnPtrType refined =
                inferredSigs.lookup(definition->getParamDecl(index))) {
          inputTypes.push_back(refined);
          continue;
        }
      // A planned cursor parameter (CTS 00204, element-generalized by
      // C99-43 slice 1) lowers to TWO inputs: a shared element slice
      // over the region and an in-out i64 cursor. The advancement
      // `*s = p` becomes a cursor write the caller observes through the
      // reference. Mutually exclusive with the inferred-fn-ptr class
      // above (different parameter types).
      if (cursorParams.contains(param)) {
        FailureOr<Type> sliceType = mapCursorParamSliceType(param);
        if (failed(sliceType))
          return failure();
        inputTypes.push_back(emitrust::RefType::get(*sliceType));
        inputTypes.push_back(
            emitrust::MutRefType::get(builder.getIntegerType(64)));
        continue;
      }
      // A planned Shape-P paired out-cursor parameter (C99-43 slice 1b)
      // lowers to ONE `&mut i64` input: the callee never touches content
      // through it, and the co-parameter's slice carries the region.
      if (pairedCursorParams.contains(param)) {
        inputTypes.push_back(
            emitrust::MutRefType::get(builder.getIntegerType(64)));
        continue;
      }
      // A planned Shape-G single-global-or-NULL out-param cursor
      // (C99-43 C1) lowers to ONE `&mut Option<i64>` in-out cell:
      // None = C NULL, Some(offset) = element offset into the plan's
      // global backing. Arity-preserving (Q1) and Option-form NULL
      // (Q4); the region itself never crosses the boundary — the
      // caller's reads resolve against the statically-known global.
      if (globalCursorParams.contains(param)) {
        inputTypes.push_back(emitrust::MutRefType::get(optionCursorType()));
        continue;
      }
      FailureOr<Type> paramType =
          mapParamType(param->getType(), translateLoc(param->getLocation()),
                       paramKinds[index], voidByteSliceElem(func, index),
                       requirementSharedConstStructParam(func, param));
      if (failed(paramType))
        return failure();
      inputTypes.push_back(*paramType);
    }
  }
  SmallVector<Type> resultTypes;
  clang::QualType returnType = func->getReturnType();
  // W2.2: a constructor is rendered as a void `&mut self` method
  // (`fn new(&mut self, ...)`) that initializes the fields in place; it has
  // no C++ return type to map (clang already reports void for one), but
  // the check is made explicit here rather than relying on that AST fact.
  if (!cxxIsCtor && !returnType->isVoidType()) {
    // FR-48: a reference RETURN is the other hard position, rejected here
    // (ahead of `mapType`'s residual) so the message names it. Returning a
    // borrow means naming the lifetime it is valid for, and the model has
    // no way to derive one: the referent may be a parameter's referent, a
    // global, or — the miscompile case — a callee local, and nothing
    // distinguishes them at the signature. Guessing an elided lifetime
    // would silently accept the dangling case, so the rejection stands
    // until an escape analysis can prove the referent outlives the call.
    if (returnType->isReferenceType())
      return emitError(loc)
             << "unsupported: reference return types are not yet supported";
    // Returning an owned stream handle would let it escape its function
    // (C99-48 v1: no escapes); checked before the data-pointer return
    // classification below.
    if (isFilePtrType(returnType))
      return emitError(loc)
             << "unsupported: FILE* cannot cross a user-defined function "
                "boundary";
    if (isDataPointer(returnType) && methodOwner &&
        ownerIndexReturns.contains(func->getCanonicalDecl())) {
      // Stage 1 owner-index return: `planOwners` proved every return site
      // roots in the same owner class as this method's own pointer
      // parameter(s), so the result is a plain i64 element index — never
      // routed through `classifyPointerReturn`, which has no
      // representation for a pointer into a parameter/callee-local region
      // (its own historical rejection for exactly this shape).
      resultTypes.push_back(builder.getIntegerType(64));
    } else if (isDataPointer(returnType)) {
      // A data-pointer return classifies by its return sites (CTS-P2):
      // the fn-address kind returns the plain fn_ptr value, and the
      // single-global-base kind (CTS-S, 00089) ERASES the result — the
      // classification is the null `Type` and the function imports
      // without one.
      FailureOr<Type> kind = classifyPointerReturn(func, loc);
      if (failed(kind))
        return failure();
      if (*kind)
        resultTypes.push_back(*kind);
    } else {
      FailureOr<Type> mapped = mapType(returnType, loc);
      if (failed(mapped))
        return failure();
      resultTypes.push_back(*mapped);
    }
  }
  // W2.24: a function in the can-throw closure returns the synthesized
  // carrier enum instead of its declared type. `planThrows` proved the
  // declared return type IS the thrown payload type (so Ok0 and Err0
  // share one payload and call sites unwrap with a single payload match);
  // the checks below are defensive nets against plan/import divergence.
  // Applied to prototypes and definitions alike — both consult the same
  // canonical-decl-keyed closure, so they build one signature.
  bool functionThrows =
      throwsPlanActive && throwsClosure.contains(func->getCanonicalDecl());
  Type throwsOkType;
  if (functionThrows) {
    FailureOr<emitrust::DataEnumType> carrier = getOrCreateThrowsEnum(loc);
    if (failed(carrier))
      return failure();
    if (resultTypes.size() != 1 ||
        resultTypes.front() != throwsPayloadMappedType)
      return emitError(loc) << "unsupported: a potentially-throwing "
                               "function must return the thrown payload type";
    throwsOkType = resultTypes.front();
    resultTypes.front() = *carrier;
  }
  FunctionType functionType = builder.getFunctionType(inputTypes, resultTypes);

  // Reconcile with an earlier import of the same symbol. Across TUs an
  // external prototype in one file is satisfied by the definition in another;
  // a second definition of the same external symbol is a duplicate. (Internal
  // statics are mangled per-TU, so any collision here is a genuine external
  // clash — for a valid single TU clang has already merged redeclarations.)
  if (func::FuncOp existing = functions.lookup(name)) {
    // Redundant declaration. FR-114 records the accepted residual hazard
    // here: two PROTOTYPE-ONLY overloads whose suffix AND MLIR signature
    // both coincide (e.g. `int f(long);` next to `int f(long long);`,
    // both `f_i64` over i64) merge silently on this path. The measured
    // backstops keep it honest: an uncalled colliding prototype emits no
    // code at all, a CALL against the wrong shape fails the argument-type
    // check, and a referenced-but-undefined survivor lands on the loud
    // whole-program "referenced but not defined in any translation unit"
    // failure (pinned in test/Import/Cpp/overload-cross-tu-asymmetric.cpp).
    if (!isDefinition)
      return success();
    if (!existing.isExternal()) {
      // W2.15: within ONE translation unit two symbols can now collide for
      // a reason the cross-TU wording describes wrongly — the finite
      // template-argument code table aliased two distinct arguments onto
      // one code (two function-pointer types, two closure types), or an
      // ordinary hand-written function already owns the composed spelling.
      // Either side of the clash may be the template one, so the check
      // asks both "is this decl an instantiation?" and "did an
      // instantiation claim this name earlier?".
      if (func->getTemplateSpecializationArgs() ||
          templateSpecSymbolNames.contains(name))
        return emitError(loc)
               << "unsupported: function template instantiation collides "
                  "with the existing symbol '"
               << name << "'";
      // FR-114: when the colliding declaration belongs to a genuine
      // same-TU overload set, say so honestly — the earlier wording blamed
      // a phantom second translation unit. The residual shapes that land
      // here are the ones the suffix table cannot split by design: the
      // frozen member `i` code (int/long, int/unsigned), same-Rust-type
      // free pairs (long/long long, double/long double), two
      // function-pointer parameters (both `px`), and separator-free
      // member code aliasing (`abi`). The wording interpolates the
      // COMPUTED symbol (raw importer spelling); pinned in
      // test/Import/Cpp/overload-collisions-invalid.cpp.
      if (isSameTUOverloadSet(func))
        return emitError(loc)
               << "unsupported: C++ overload set for '"
               << func->getDeclName().getAsString()
               << "' maps two overloads onto one emitted symbol '" << name
               << "' (parameter types not distinguishable in the overload "
                  "suffix)";
      return emitError(loc)
             << "unsupported: conflicting definition of '" << name
             << "' (already defined in another translation unit)";
    }
    if (existing.getFunctionType() != functionType) {
      // A definition may refine a prototype-only import's pointer
      // parameters from scalar references to slices (the prototype's TU
      // could not see the body). The refinement is only sound while no
      // call was imported against the scalar shape.
      if (!isSliceRefinementOf(existing.getFunctionType(), functionType))
        return emitError(loc)
               << "unsupported: conflicting redeclaration of '" << name
               << "'";
      if (!SymbolTable::symbolKnownUseEmpty(existing.getOperation(),
                                            module.getOperation()))
        return emitError(loc)
               << "unsupported: function '" << name << "' was called as "
               << existing.getFunctionType()
               << " before its definition refined the signature to "
               << functionType
               << " (cross-TU pointer-parameter classification)";
    }
    // The erased declaration is always EXTERNAL (the non-external case
    // returned above), i.e. a body-less prototype. Recoverable import
    // (FR-42) keeps a clone of it so that, if this definition then rejects,
    // a call already imported against the prototype does not lose its
    // callee; `eraseTopLevelOp` also keeps the live checkpoint's anchor
    // valid when the prototype happens to be the last module operation.
    if (activeCheckpoint)
      activeCheckpoint->erasedExternalClones.push_back(
          existing.getOperation()->clone());
    eraseTopLevelOp(existing.getOperation());
    functions.erase(name);
  }

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(module.getBody());
  auto funcOp = builder.create<func::FuncOp>(loc, name, functionType);
  if (methodOwner)
    funcOp->setAttr(emitrust::kMethodOfAttrName,
                    builder.getStringAttr(ownerStructType.getName()));
  // W2.2: a genuine C++ method places into its class's impl exactly like a
  // Phase-4 owner method; a static method additionally carries the
  // `static_method` marker so `emitrust.impl`'s verifier and the Rust
  // emitter know not to expect (and not to render) a receiver argument.
  if (cxxMethod) {
    funcOp->setAttr(emitrust::kMethodOfAttrName,
                    builder.getStringAttr(cxxOwnerStructType.getName()));
    if (!cxxHasReceiver)
      funcOp->setAttr(emitrust::kStaticMethodAttrName, builder.getUnitAttr());
    // W2.17: the destructor body becomes `impl Drop for <Struct>`'s single
    // member. The marker is what `convert-func-to-emitrust` keys on to
    // route it into a SECOND, trait-carrying impl and rename it to `drop`.
    if (llvm::isa<clang::CXXDestructorDecl>(cxxMethod))
      funcOp->setAttr(emitrust::kDropImplAttrName, builder.getUnitAttr());
  }
  functions[name] = funcOp;
  // Recovery stub retry (FR-42): the signature above is the one the real
  // import would have used — same pointer-parameter classification, same
  // owner/method placement, same mangled name — so every call site that
  // survives still type-checks against it. Only the body is replaced.
  if (recoveryStubOnly) {
    recoveryStubSymbol = name;
    return emitRecoveryStub(funcOp, loc);
  }
  if (!isDefinition) {
    funcOp.setPrivate();
    return success();
  }

  // Function prologue: reset per-function state, then materialize each
  // parameter as a place appropriate to its kind.
  symbols.clear();
  addressTaken.clear();
  fileLocals.clear();
  pointerLocals.clear();
  stringViewLocals.clear();
  pointerPointerLocals.clear();
  carrierLocals.clear();
  carrierParams.clear();
  literalBackings.clear();
  paramCells.clear();
  ownerStructPlaces.clear();
  loopStack.clear();
  labelBlocks.clear();
  switchCaseBlocks.clear();
  // W2.13: recognized lambda locals are per-function; the pending-body
  // queue is drained at the end of THIS import, so any entry still here
  // is a leftover of a rolled-back rejection (FR-42) whose FuncOp no
  // longer exists — dropping it is the only safe disposition.
  lambdaLocals.clear();
  pendingLiftedLambdas.clear();
  inferredFnPtrSigs = std::move(inferredSigs);
  cursorWritebacks.clear();
  pairedCursorPlaces.clear();
  globalCursorPlaces.clear();
  currentVaCloneActive = false;
  currentVaExtras.clear();
  currentVaCursorCell = Value();
  currentHasLabels = containsLabelStmt(func->getBody());
  currentFunctionBody = func->getBody();
  placeBackedScalars.clear();
  inductionValues.clear();
  currentReceiverPlace = Value();
  currentPoolPlace = Value();
  currentMethodOwner = nullptr;
  currentOwnerIndexReturn =
      methodOwner && ownerIndexReturns.contains(func->getCanonicalDecl());
  currentCxxThisRef = Value();
  currentReturnType = resultTypes.empty() ? Type() : resultTypes.front();
  // W2.24: `emitReturnStmt` wraps a closure member's declared return value
  // in Ok0; try/catch is admitted only in TU-level functions (wave-1
  // gate), and both stacks are per-function.
  currentFunctionThrows = functionThrows;
  currentThrowsOkType = throwsOkType;
  currentTryAllowed = !cxxMethod;
  tryContexts.clear();
  catchPayloadPlaces.clear();
  currentFamOptionPayload = famOptionReturnPayload(func, currentReturnType);
  famOptionTemps.clear();
  // An erased single-global-base pointer return (CTS-S, 00089): return
  // sites emit a bare `return` instead of the classified `&global`.
  currentErasedReturnBase =
      globalReturnBases.lookup(func->getCanonicalDecl());
  currentFuncName = name;
  currentIsMain = name == "c_main";
  mainArgvTableValue = Value();
  bodyRegion = &funcOp.getBody();
  entryBlock = funcOp.addEntryBlock();
  builder.setInsertionPointToStart(entryBlock);
  collectAddressTaken(func->getBody());
  // FR-61f: mark body-touched scalars of range-eligible loops as places.
  collectRangeForPlaceScalars(func->getBody());
  // Calls to carrier-returning functions are carrier sources of the
  // assigned pointer's region (CTS-P3).
  pointerRegions.carrierReturnQuery =
      [this](const clang::FunctionDecl *callee) {
        return isCarrierReturnFunction(callee);
      };
  // Calls to owner-index-returning methods (Stage 1) are region sources
  // exactly like a copy from one of the callee's own pointer parameters:
  // `recordPointerWrite` re-classifies the call's first pointer argument.
  pointerRegions.ownerIndexReturnQuery =
      [this](const clang::FunctionDecl *callee) {
        return ownerIndexReturns.contains(callee->getCanonicalDecl());
      };
  // A local assigned from an array-member field read (Stage 4, B3, e.g.
  // `parent = node->parent;`) joins the arrow base's owner class; by
  // emission time every field's `arrayMemberPtrBindings` entry (if any)
  // is fully proven, so the query is exact membership, not the structural
  // candidacy Pass A itself used while still proving it.
  pointerRegions.arrayMemberFieldQuery =
      [this](const clang::FieldDecl *field) {
        auto it = arrayMemberPtrBindings.find(field);
        return it != arrayMemberPtrBindings.end() &&
               it->second.invalidReason.empty();
      };
  // Admitted local `void *` fn-ptr holders (CTS-F, 00210) import as
  // ordinary fn_ptr locals; the pointer decomposition never tracks them.
  collectVoidFnPtrHolders(func->getBody());
  pointerRegions.fnHolderQuery = [this](const clang::VarDecl *var) {
    return voidFnPtrHolders.contains(var);
  };
  pointerRegions.stringValueLocalQuery = [this](const clang::VarDecl *var) {
    return stringFillLocals.contains(var);
  };
  pointerRegions.vecValueLocalQuery = [this](const clang::VarDecl *var) {
    return vecValueLocals.contains(var);
  };
  // FR-94: FAM-record owned-tail locals bypass the region model entirely
  // (the owned struct binding replaces the decomposition), and the
  // `&d->tail[k]` binding arm gates on the admitted FAM leaf.
  pointerRegions.famValueLocalQuery = [this](const clang::VarDecl *var) {
    return famAllocLocals.contains(var);
  };
  pointerRegions.famTailMemberQuery =
      [this](const clang::MemberExpr *member) {
        const auto *leaf =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
        return leaf && famTailField(leaf->getParent()) == leaf;
      };
  // FR-96: member-read locals over lifted member-held FAM fields bypass
  // the region model (every use is a fresh Option projection), and a
  // recognized member write is owned by the Option-member machinery —
  // the same predicate planOwners used, so the poison set and the
  // emission agree.
  pointerRegions.famMemberLocalQuery = [this](const clang::VarDecl *var) {
    return famMemberLocals.contains(var);
  };
  pointerRegions.famMemberWriteQuery =
      [this, func](const clang::FieldDecl *field, const clang::Expr *rhs) {
        return famMemberLiftRecognizes(func, field, rhs);
      };
  // FR-93: member-array decays bound to pointer locals classify through
  // the shared admission (typed member-place backings and byte-region
  // window roots); the classifier itself gates the C++ path off.
  pointerRegions.memberArrayDecayQuery =
      [this](const clang::MemberExpr *member) {
        return classifyMemberArrayDecay(member);
      };
  pointerRegions.literalTemps = &literalTemps;
  // String-cursor parameters (CTS 00204): the walk binds `p = *s` to the
  // parameter's region and lets `&p` arguments to cursor positions pass
  // without invalidation.
  pointerRegions.cursorParamQuery = [this](const clang::ParmVarDecl *param) {
    return cursorParams.contains(param);
  };
  pointerRegions.cursorArgQuery = [this](const clang::FunctionDecl *callee,
                                         unsigned index) {
    const clang::FunctionDecl *definition = callee->getDefinition();
    if (!definition || index >= definition->getNumParams())
      return false;
    return cursorParams.contains(definition->getParamDecl(index));
  };
  // Shape-P paired out-cursor positions (C99-43 slice 1b): a `&e`
  // argument there joins `e` into the co-argument's region.
  pointerRegions.pairedArgQuery = [this](const clang::FunctionDecl *callee,
                                         unsigned index) -> int {
    const clang::FunctionDecl *definition = callee->getDefinition();
    if (!definition || index >= definition->getNumParams())
      return -1;
    const clang::ParmVarDecl *coParam =
        pairedCursorParams.lookup(definition->getParamDecl(index));
    if (!coParam)
      return -1;
    for (unsigned coIndex = 0; coIndex < definition->getNumParams();
         ++coIndex)
      if (definition->getParamDecl(coIndex) == coParam)
        return static_cast<int>(coIndex);
    return -1;
  };
  // Shape-G single-global-or-NULL positions (C99-43 C1): a `&p`
  // argument there is consumed by the Option-cell staging, binds the
  // plan's global as p's region base, and a null-writing callee marks
  // the region nullable.
  pointerRegions.globalCursorArgQuery =
      [this](const clang::FunctionDecl *callee,
             unsigned index) -> std::optional<GlobalCursorPlan> {
    const clang::FunctionDecl *definition = callee->getDefinition();
    if (!definition || index >= definition->getNumParams())
      return std::nullopt;
    auto it = globalCursorParams.find(definition->getParamDecl(index));
    if (it == globalCursorParams.end())
      return std::nullopt;
    return it->second;
  };
  pointerRegions.analyze(astContext(), func->getBody());

  // Method prologue (Phase 4): the receiver dereferences once into the
  // owner struct place, whose "data" member is the region base every
  // pointer parameter (and every pointer local unified with one)
  // decomposes against.
  Value receiverDataPlace;
  if (methodOwner) {
    Value receiverArg = entryBlock->getArgument(0);
    Value receiverPlace =
        builder
            .create<emitrust::DerefOp>(
                loc, emitrust::LValueType::get(ownerStructType), receiverArg)
            .getResult();
    FailureOr<Type> ownedType = mapType(methodOwner->getType(), loc);
    if (failed(ownedType))
      return failure();
    receiverDataPlace = builder
                            .create<emitrust::MemberOp>(
                                loc, emitrust::LValueType::get(*ownedType),
                                receiverPlace, builder.getStringAttr("data"))
                            .getResult();
    currentReceiverPlace = receiverPlace;
    currentMethodOwner = methodOwner;
  }
  // W2.2: a genuine C++ instance method's receiver stays an undereferenced
  // ref/mut_ref value — `CXXThisExpr` resolves directly to it, and the
  // existing `->`-base rvalue path (`emitMemberBasePlace`) derefs it lazily
  // at each member access, exactly like any other pointer-typed base
  // expression.
  if (cxxHasReceiver)
    currentCxxThisRef = entryBlock->getArgument(0);

  // FR-61e slice 2: one `emitrust.param_names` slot per signature input.
  // A slot stays empty when the C parameter is unnamed, when its value is
  // copied into a named shadow variable (bindOrdinaryParam's by-value
  // path -- the shadow already took the spelling, and two bindings must
  // never share one), for the method receiver (self is hard-wired), and
  // for the synthesized cursor input of a Shape-S cursor parameter (a
  // Shape-P paired out-cursor's single input IS named after its C
  // parameter, so the callee reads `*endp = ...`).
  SmallVector<Attribute> paramNameSlots(functionType.getNumInputs(),
                                        builder.getStringAttr(""));
  auto paramSlotName = [&](const clang::ParmVarDecl *param,
                           Type argType) -> std::string {
    if (param->getName().empty())
      return {};
    bool isRef =
        llvm::isa<emitrust::MutRefType, emitrust::RefType>(argType);
    bool shadowed =
        !isRef && (llvm::isa<emitrust::StructType, emitrust::EnumType,
                             emitrust::FnPtrType, emitrust::OpaqueType>(
                       argType) ||
                   isUnsignedInt(argType) || addressTaken.contains(param));
    if (shadowed)
      return {};
    return mangleMemberName(param->getName());
  };
  unsigned entryArgIndex = (methodOwner || cxxHasReceiver) ? 1 : 0;
  for (const clang::ParmVarDecl *param : func->parameters()) {
    // main's `argv`: an admitted parameter (C99-43 C3) owns the
    // `!emitrust.argv_table` entry-block argument, named after its C
    // spelling so the signature reads `argv: &[Vec<i8>]`; otherwise it
    // was dropped from the imported signature (no entry-block argument;
    // uses were rejected at signature time, so no binding is needed).
    if (currentIsMain && isPointerType(param->getType())) {
      if (param == mainArgvAdmittedParam) {
        Value tableArg = entryBlock->getArgument(entryArgIndex++);
        if (!param->getName().empty())
          paramNameSlots[entryArgIndex - 1] =
              builder.getStringAttr(mangleMemberName(param->getName()));
        bindArgvParam(param, tableArg);
      }
      continue;
    }
    Location paramLoc = translateLoc(param->getLocation());
    // A string-cursor parameter (CTS 00204) owns TWO entry-block
    // arguments: the shared region slice and the in-out cursor.
    if (cursorParams.contains(param)) {
      Value baseArg = entryBlock->getArgument(entryArgIndex);
      Value cursorArg = entryBlock->getArgument(entryArgIndex + 1);
      entryArgIndex += 2;
      // The base slice reads back as `(*name)[..]`; the cursor stays vN.
      if (!param->getName().empty())
        paramNameSlots[entryArgIndex - 2] =
            builder.getStringAttr(mangleMemberName(param->getName()));
      if (failed(bindCursorParam(param, baseArg, cursorArg, paramLoc)))
        return failure();
      continue;
    }
    // A Shape-P paired out-cursor parameter (C99-43 slice 1b) owns ONE
    // entry-block argument: the `&mut i64` out-cursor, deref'd once and
    // assigned exactly once by the admitted write — no cell, no
    // writeback. Its slot is named after the C parameter (mirroring the
    // S base-slot naming) so the callee reads `*endp = ...`.
    if (pairedCursorParams.contains(param)) {
      Value cursorArg = entryBlock->getArgument(entryArgIndex++);
      if (!param->getName().empty())
        paramNameSlots[entryArgIndex - 1] =
            builder.getStringAttr(mangleMemberName(param->getName()));
      Value place = builder
                        .create<emitrust::DerefOp>(
                            paramLoc,
                            emitrust::LValueType::get(
                                builder.getIntegerType(64)),
                            cursorArg)
                        .getResult();
      pairedCursorPlaces[param] = place;
      continue;
    }
    // A Shape-G single-global-or-NULL out-param cursor (C99-43 C1)
    // owns ONE entry-block argument: the `&mut Option<i64>` cell,
    // deref'd once and assigned exactly once by the admitted write.
    // Named after the C parameter so the callee reads `*efp = ...`.
    if (globalCursorParams.contains(param)) {
      Value cellArg = entryBlock->getArgument(entryArgIndex++);
      if (!param->getName().empty())
        paramNameSlots[entryArgIndex - 1] =
            builder.getStringAttr(mangleMemberName(param->getName()));
      Value place =
          builder
              .create<emitrust::DerefOp>(
                  paramLoc, emitrust::LValueType::get(optionCursorType()),
                  cellArg)
              .getResult();
      globalCursorPlaces[param] = place;
      continue;
    }
    Value blockArg = entryBlock->getArgument(entryArgIndex++);
    paramNameSlots[entryArgIndex - 1] =
        builder.getStringAttr(paramSlotName(param, blockArg.getType()));
    // An integer-carrier `void *` parameter (CTS-P3) is a plain i64
    // scalar; remember it so truth tests and carrier reads route to its
    // prologue cell (bound through the ordinary scalar path below).
    if (!methodOwner && isDataPointer(param->getType()) &&
        blockArg.getType() == builder.getIntegerType(64))
      carrierParams.insert(param);
    if (methodOwner && isPointerType(param->getType()) &&
        !isFunctionPointer(param->getType())) {
      // Owner-region pointer parameter: an i64 element index into the
      // receiver's array, decomposed exactly like a slice parameter with
      // the receiver's data member as region base and the index argument
      // as the initial cursor.
      Value cursorCell =
          createEntryAlloca(paramLoc, builder.getIntegerType(64));
      builder.create<memref::StoreOp>(paramLoc, blockArg, cursorCell);
      symbols[param] = receiverDataPlace;
      pointerLocals[param] = PointerLocalInfo{param, cursorCell};
      continue;
    }
    if (failed(bindOrdinaryParam(param, blockArg, paramLoc)))
      return failure();
  }
  // An all-empty slot array carries no information; omit it so no-param
  // and fully-shadowed signatures keep their attribute dicts unchanged.
  if (llvm::any_of(paramNameSlots, [](Attribute slot) {
        return !cast<StringAttr>(slot).getValue().empty();
      }))
    funcOp->setAttr(emitrust::kParamNamesAttrName,
                    builder.getArrayAttr(paramNameSlots));

  // W2.2: a constructor's member-initializer list (`CXXCtorInitializer`s,
  // which live OUTSIDE `getBody()`) lowers to ordinary member assignments
  // against the (always-mutable) receiver, in declaration order, AHEAD OF
  // the constructor's own compound-statement body: `deref(self)`,
  // `member(field)`, `assign`. No implicit zero-fill precedes it (this
  // wave's classes never mix a member-initializer list with fields it
  // omits).
  if (cxxIsCtor) {
    const auto *ctorDecl = llvm::cast<clang::CXXConstructorDecl>(func);
    // A member-initializer that reads one of the constructor's OWN
    // parameters binds the incoming parameter directly — the raw
    // entry-block argument — rather than the memref cell
    // `bindOrdinaryParam` spilled it into for the (unrelated) body below;
    // constructor parameters are always plain scalars (never a cursor or
    // owner-region pointer parameter), so they sit at a fixed one-to-one
    // offset from the receiver argument.
    llvm::DenseMap<const clang::ParmVarDecl *, Value> rawCtorParamArgs;
    for (auto [index, param] : llvm::enumerate(func->parameters()))
      rawCtorParamArgs[param] = entryBlock->getArgument(1 + index);
    for (const clang::CXXCtorInitializer *init : ctorDecl->inits()) {
      // W2.18: a BASE initializer (`Derived(int a, int b) : Base(a), y(b)`)
      // is a `CXXCtorInitializer` with `isBaseInitializer()`, not a field
      // one, and its child is a `CXXConstructExpr` for the base's
      // constructor. With the base as a first field it is exactly a
      // constructor call against `self.base`, which is what
      // `emitCXXConstructInit` already builds (`addr_of mut` + a
      // `emitrust.method_call`-tagged `func.call`). Deliberately NOT routed
      // through `emitRValue` like a field initializer: measured, that path
      // rejects any class-typed initializer as `unsupported: constructor in
      // value position`.
      if (init->isBaseInitializer()) {
        Location baseLoc = translateLoc(init->getSourceLocation());
        const auto *baseConstruct =
            llvm::dyn_cast<clang::CXXConstructExpr>(init->getInit());
        if (!baseConstruct)
          return emitError(baseLoc)
                 << "unsupported: base constructor initializer";
        const clang::CXXRecordDecl *baseRecord =
            init->getBaseClass()->getAsCXXRecordDecl();
        if (baseRecord && baseRecord->hasDefinition() && baseRecord->isEmpty()) {
          // An EMPTY base has no `base` field to construct into (see
          // `collectRecordFields`). A TRIVIAL construction of it has no
          // observable effect, so there is nothing to emit; a user-provided
          // constructor body would be silently DROPPED, so it rejects.
          const clang::CXXConstructorDecl *baseCtor =
              baseConstruct->getConstructor();
          if (baseCtor && baseCtor->isTrivial())
            continue;
          return emitError(baseLoc)
                 << "unsupported: constructor of an empty base class";
        }
        FailureOr<Type> baseFieldType =
            mapType(init->getBaseClass()->getCanonicalTypeInternal(), baseLoc);
        if (failed(baseFieldType))
          return failure();
        Value baseSelfPlace =
            builder
                .create<emitrust::DerefOp>(
                    baseLoc, emitrust::LValueType::get(cxxOwnerStructType),
                    currentCxxThisRef)
                .getResult();
        Value basePlace =
            builder
                .create<emitrust::MemberOp>(
                    baseLoc, emitrust::LValueType::get(*baseFieldType),
                    baseSelfPlace, builder.getStringAttr("base"))
                .getResult();
        if (failed(emitCXXConstructInit(basePlace, baseConstruct, baseLoc)))
          return failure();
        continue;
      }
      if (!init->isMemberInitializer())
        return emitError(loc)
               << "unsupported: non-member constructor initializer";
      const clang::FieldDecl *field = init->getMember();
      Location initLoc = translateLoc(init->getSourceLocation());
      Value selfPlace =
          builder
              .create<emitrust::DerefOp>(
                  initLoc, emitrust::LValueType::get(cxxOwnerStructType),
                  currentCxxThisRef)
              .getResult();
      FailureOr<Type> fieldType =
          mapType(flattenedFieldStorage(field)->getType(), initLoc);
      if (failed(fieldType))
        return failure();
      Value fieldPlace = builder
                             .create<emitrust::MemberOp>(
                                 initLoc, emitrust::LValueType::get(*fieldType),
                                 selfPlace,
                                 builder.getStringAttr(flattenedFieldName(field)))
                             .getResult();
      const clang::Expr *initExpr = init->getInit()->IgnoreParenImpCasts();
      Value rawParamArg;
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(initExpr))
        if (const auto *param =
                llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()))
          rawParamArg = rawCtorParamArgs.lookup(param);
      FailureOr<Value> initValue =
          rawParamArg ? FailureOr<Value>(rawParamArg)
                      : emitRValue(init->getInit());
      if (failed(initValue))
        return failure();
      if ((*initValue).getType() != *fieldType)
        return emitError(initLoc)
               << "unsupported: constructor initializer type mismatch";
      builder.create<emitrust::AssignOp>(initLoc, fieldPlace, *initValue);
    }
  }

  // W4.2e Part B (FR-39): a promoting function emits its node pool at entry
  // as the high-level, backend-agnostic `emitrust.collection` place (element
  // type + folded capacity); every handle's member projection is a
  // `collection_at` and malloc a `collection_push`. The
  // emitrust-lower-containers pass turns these back into the concrete fixed
  // `[T; cap]` array + i64 free cursor before the rest of the pipeline runs.
  if (auto poolIt = mallocPools.find(func->getCanonicalDecl());
      poolIt != mallocPools.end()) {
    const MallocPoolFacts &facts = poolIt->second;
    FailureOr<Type> elementType =
        mapType(astContext().getRecordType(facts.structDecl), loc);
    if (failed(elementType))
      return failure();
    Type collectionType = emitrust::OpaqueType::get(builder.getContext(),
                                                    "__emitrust_collection");
    currentPoolPlace =
        builder
            .create<emitrust::CollectionOp>(
                loc, emitrust::LValueType::get(collectionType),
                TypeAttr::get(*elementType), builder.getI64IntegerAttr(facts.cap))
            .getResult();
  }

  if (failed(emitStmt(func->getBody())))
    return failure();
  if (failed(finalizeFunction(funcOp, loc)))
    return failure();
  // W2.13: lifted lambda bodies import only now — the shared body
  // emitter's per-function state is single-occupancy, so they had to
  // wait for this function's own emission to finish.
  return importPendingLiftedLambdas();
}

LogicalResult CImporter::importLiftedLambda(
    const clang::VarDecl *var, const clang::LambdaExpr *lambda,
    SmallVector<Value, 4> frozenCaptures,
    SmallVector<const clang::VarDecl *, 4> captures, Location loc) {
  const clang::CXXMethodDecl *callOperator = lambda->getCallOperator();
  // The block-scope `<function>_<name>` mangle (the static-local
  // convention, function spelling): a lambda local is a block-scope
  // entity surfacing at module level, so it takes the enclosing
  // function's (already-mangled) name as its prefix. Deliberately NOT
  // modeled by the FR-40 item graph, like every block-scope mangle.
  std::string name = fnRustName(
      (llvm::Twine(currentFuncName) + "_" + var->getName()).str());
  if (functions.lookup(name) || ordinaryNameTaken(name))
    return emitError(loc) << "unsupported: lambda '" << var->getName()
                          << "' lifts to '" << name
                          << "', which collides with an existing symbol";
  // Signature: the frozen captures PREPENDED, then the operator()'s own
  // parameters. Types map through the plain value mapping — a parameter
  // shape only `mapParamType`'s pointer classification could admit keeps
  // a located rejection here.
  SmallVector<Type> inputTypes;
  for (const clang::VarDecl *capture : captures) {
    FailureOr<Type> type =
        mapType(capture->getType().getCanonicalType(), loc);
    if (failed(type))
      return failure();
    inputTypes.push_back(*type);
  }
  for (const clang::ParmVarDecl *param : callOperator->parameters()) {
    FailureOr<Type> type = mapType(param->getType().getCanonicalType(),
                                   translateLoc(param->getLocation()));
    if (failed(type))
      return failure();
    inputTypes.push_back(*type);
  }
  SmallVector<Type> resultTypes;
  clang::QualType returnType = callOperator->getReturnType();
  if (!returnType->isVoidType()) {
    // Mirrors importFunction's reference-return rejection (FR-48): no
    // lifetime can be derived at the signature.
    if (returnType->isReferenceType())
      return emitError(loc)
             << "unsupported: reference return types are not yet supported";
    FailureOr<Type> mapped = mapType(returnType, loc);
    if (failed(mapped))
      return failure();
    resultTypes.push_back(*mapped);
  }
  FunctionType functionType = builder.getFunctionType(inputTypes, resultTypes);
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(module.getBody());
  auto funcOp = builder.create<func::FuncOp>(loc, name, functionType);
  functions[name] = funcOp;
  lambdaLocals[var] =
      LambdaLocalInfo{funcOp, std::move(frozenCaptures), callOperator};
  pendingLiftedLambdas.push_back(
      PendingLiftedLambda{funcOp, callOperator, std::move(captures)});
  return success();
}

LogicalResult CImporter::importPendingLiftedLambdas() {
  // A lambda declared inside a lifted body re-queues, so drain until
  // empty rather than iterating a snapshot.
  while (!pendingLiftedLambdas.empty()) {
    PendingLiftedLambda pending = std::move(pendingLiftedLambdas.front());
    pendingLiftedLambdas.erase(pendingLiftedLambdas.begin());
    if (failed(importLiftedLambdaBody(pending)))
      return failure();
  }
  return success();
}

LogicalResult
CImporter::importLiftedLambdaBody(const PendingLiftedLambda &pending) {
  func::FuncOp funcOp = pending.funcOp;
  const clang::CXXMethodDecl *callOperator = pending.callOperator;
  const clang::Stmt *body = callOperator->getBody();
  Location loc = translateLoc(callOperator->getBeginLoc());
  OpBuilder::InsertionGuard guard(builder);

  // Function prologue: the same per-function state reset importFunction
  // performs (mirroring emitVaClone's secondary prologue — the enclosing
  // function's emission is already finalized when this runs, so nothing
  // needs saving). The pointerRegions query wiring installed by the
  // enclosing importFunction captures only `this` and program-wide plan
  // maps, so it stays valid across the re-analysis.
  symbols.clear();
  addressTaken.clear();
  fileLocals.clear();
  pointerLocals.clear();
  stringViewLocals.clear();
  lambdaLocals.clear();
  pointerPointerLocals.clear();
  carrierLocals.clear();
  carrierParams.clear();
  literalBackings.clear();
  paramCells.clear();
  ownerStructPlaces.clear();
  loopStack.clear();
  labelBlocks.clear();
  switchCaseBlocks.clear();
  inferredFnPtrSigs.clear();
  cursorWritebacks.clear();
  pairedCursorPlaces.clear();
  globalCursorPlaces.clear();
  currentVaCloneActive = false;
  currentVaExtras.clear();
  currentVaCursorCell = Value();
  currentHasLabels = containsLabelStmt(body);
  currentFunctionBody = body;
  placeBackedScalars.clear();
  inductionValues.clear();
  currentReceiverPlace = Value();
  currentPoolPlace = Value();
  currentMethodOwner = nullptr;
  currentOwnerIndexReturn = false;
  currentCxxThisRef = Value();
  FunctionType functionType = funcOp.getFunctionType();
  currentReturnType =
      functionType.getNumResults() ? functionType.getResult(0) : Type();
  // W2.24: a lifted lambda is never in the can-throw closure and admits
  // no try/catch; a throw inside one rejects loudly.
  currentFunctionThrows = false;
  currentThrowsOkType = Type();
  currentTryAllowed = false;
  tryContexts.clear();
  catchPayloadPlaces.clear();
  currentFamOptionPayload = Type(); // FR-99: no C++ lambda is a FAM allocator.
  famOptionTemps.clear();
  currentErasedReturnBase = nullptr;
  currentFuncName = funcOp.getSymName().str();
  currentIsMain = false;
  mainArgvTableValue = Value();
  bodyRegion = &funcOp.getBody();
  entryBlock = funcOp.addEntryBlock();
  builder.setInsertionPointToStart(entryBlock);
  collectAddressTaken(body);
  collectRangeForPlaceScalars(body);
  collectVoidFnPtrHolders(body);
  pointerRegions.analyze(astContext(), body);

  // Bind the capture VarDecls (prepended arguments) and the operator()'s
  // ParmVarDecls (trailing arguments) into `symbols`: the operator()
  // body's DeclRefExprs point at the ENCLOSING VarDecls, not closure
  // fields, so these bindings are the entire capture rewrite. Slot
  // naming mirrors importFunction's paramSlotName (empty when the value
  // is copied into a named shadow variable, which then owns the
  // spelling).
  SmallVector<Attribute> paramNameSlots(functionType.getNumInputs(),
                                        builder.getStringAttr(""));
  auto slotName = [&](const clang::VarDecl *decl, Type argType) -> std::string {
    if (decl->getName().empty())
      return {};
    bool isRef =
        llvm::isa<emitrust::MutRefType, emitrust::RefType>(argType);
    bool shadowed =
        !isRef && (llvm::isa<emitrust::StructType, emitrust::EnumType,
                             emitrust::FnPtrType>(argType) ||
                   isUnsignedInt(argType) || addressTaken.contains(decl));
    if (shadowed)
      return {};
    return mangleMemberName(decl->getName());
  };
  unsigned entryArgIndex = 0;
  for (const clang::VarDecl *capture : pending.captures) {
    Value blockArg = entryBlock->getArgument(entryArgIndex);
    paramNameSlots[entryArgIndex] =
        builder.getStringAttr(slotName(capture, blockArg.getType()));
    ++entryArgIndex;
    if (failed(bindLiftedCaptureValue(capture, blockArg, loc)))
      return failure();
  }
  for (const clang::ParmVarDecl *param : callOperator->parameters()) {
    Value blockArg = entryBlock->getArgument(entryArgIndex);
    paramNameSlots[entryArgIndex] =
        builder.getStringAttr(slotName(param, blockArg.getType()));
    ++entryArgIndex;
    if (failed(bindOrdinaryParam(param, blockArg,
                                 translateLoc(param->getLocation()))))
      return failure();
  }
  if (llvm::any_of(paramNameSlots, [](Attribute slot) {
        return !cast<StringAttr>(slot).getValue().empty();
      }))
    funcOp->setAttr(emitrust::kParamNamesAttrName,
                    builder.getArrayAttr(paramNameSlots));

  if (failed(emitStmt(body)))
    return failure();
  return finalizeFunction(funcOp, loc);
}

LogicalResult CImporter::bindLiftedCaptureValue(const clang::VarDecl *var,
                                                Value blockArg, Location loc) {
  Type type = blockArg.getType();
  // Mirrors bindOrdinaryParam's by-value branches for the scalar shapes
  // the capture gate admits (bindOrdinaryParam itself takes a
  // ParmVarDecl, which a captured local is not). Unsigned scalars and
  // address-taken captures copy into a named `emitrust.variable` shadow
  // (a memref cell of either would break mem2reg — see bindOrdinaryParam);
  // plain signed/float scalars take a promotable rank-0 cell.
  if (isUnsignedInt(type) || addressTaken.contains(var)) {
    Value place = builder
                      .create<emitrust::VariableOp>(
                          loc, emitrust::LValueType::get(type),
                          /*init=*/Attribute(), /*isConst=*/false,
                          var->getName().empty()
                              ? std::string()
                              : mangleMemberName(var->getName()))
                      .getResult();
    builder.create<emitrust::AssignOp>(loc, place, blockArg);
    symbols[var] = place;
    return success();
  }
  Value cell = createEntryAlloca(loc, type);
  builder.create<memref::StoreOp>(loc, blockArg, cell);
  symbols[var] = cell;
  paramCells.push_back(cell);
  return success();
}

LogicalResult CImporter::bindOrdinaryParam(const clang::ParmVarDecl *param,
                                           Value blockArg, Location paramLoc) {
  Type type = blockArg.getType();
  {
    Type refPointee;
    if (auto mutRef = llvm::dyn_cast<emitrust::MutRefType>(type))
      refPointee = mutRef.getPointee();
    else if (auto sharedRef = llvm::dyn_cast<emitrust::RefType>(type))
      refPointee = sharedRef.getPointee();
    if (auto sliceType =
            llvm::dyn_cast_or_null<emitrust::SliceType>(refPointee)) {
      // Slice parameter (Phase 1b): one entry-block dereference
      // establishes the region base place, and the parameter itself
      // decomposes into (base, i64 cursor = 0) exactly like a decayed
      // local array; every element access renders `(*param)[i as usize]`
      // so no borrow is ever held across statements. A shared slice
      // (`&[u8]`, the const byte-region walkers) decomposes the same
      // way; writes through it were excluded by the const pointee.
      Value basePlace =
          builder
              .create<emitrust::DerefOp>(
                  paramLoc, emitrust::LValueType::get(sliceType), blockArg)
              .getResult();
      Value cursorCell =
          createEntryAlloca(paramLoc, builder.getIntegerType(64));
      Value zero = createIntConstant(paramLoc, builder.getIntegerType(64), 0);
      builder.create<memref::StoreOp>(paramLoc, zero, cursorCell);
      symbols[param] = basePlace;
      pointerLocals[param] = PointerLocalInfo{param, cursorCell};
      return success();
    }
  }
  if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(type)) {
    // Scalar-reference pointer parameter: used directly as a reference
    // SSA value.
    symbols[param] = blockArg;
    return success();
  }
  if (llvm::isa<emitrust::StructType, emitrust::EnumType,
                emitrust::FnPtrType, emitrust::OpaqueType>(type) ||
      isUnsignedInt(type) || addressTaken.contains(param)) {
    // By-value struct, enum, function pointer, opaque (FR-88's
    // `Option<&[u8]>` nullable byte-slice parameter), or unsigned
    // scalar, or an address-taken scalar: copy into a Rust variable
    // (dialect-typed values must not become memref cells — a memref of a
    // dialect type is illegal — and unsigned cells must not either,
    // because mem2reg materializes its default value as an
    // `arith.constant`, which requires a signless type). FR-61e: the
    // shadow carries the parameter's final spelling; an unnamed
    // parameter stays anonymous.
    Value place = builder
                      .create<emitrust::VariableOp>(
                          paramLoc, emitrust::LValueType::get(type),
                          /*init=*/Attribute(), /*isConst=*/false,
                          param->getName().empty()
                              ? std::string()
                              : mangleMemberName(param->getName()))
                      .getResult();
    builder.create<emitrust::AssignOp>(paramLoc, place, blockArg);
    symbols[param] = place;
    // FR-88: the named lvalue place is the method-call receiver for the
    // parameter's null tests (`is_some`/`is_none`) and per-use-site
    // `unwrap`s; the set routes those interceptions and, critically,
    // BYPASSES the statically-non-null comparison fold — `None` call
    // sites are legal for this class, so folding its null test would be
    // a miscompile, not a rejection.
    if (isNullableByteSliceType(type) && isDataPointer(param->getType()))
      nullableByteParams.insert(param);
    return success();
  }
  if (llvm::isa<emitrust::ArrayType>(type))
    return emitError(paramLoc) << "unsupported: array parameter";
  // Plain scalar: promotable rank-0 memref cell (swept by
  // `finalizeFunction` when the parameter is never read).
  Value cell = createEntryAlloca(paramLoc, type);
  builder.create<memref::StoreOp>(paramLoc, blockArg, cell);
  symbols[param] = cell;
  paramCells.push_back(cell);
  return success();
}

FailureOr<Type> CImporter::mapCursorParamSliceType(
    const clang::ParmVarDecl *param) {
  // The element run a `T **` cursor parameter walks is a run of T; the
  // element maps exactly like any other value position (char keeps the
  // historical i8 byte slice), and an element the slice type cannot
  // view keeps a located rejection at the signature.
  Location loc = translateLoc(param->getLocation());
  FailureOr<Type> element =
      mapType(pointerPointerElementType(param->getType()), loc);
  if (failed(element))
    return failure();
  if (!emitrust::SliceType::isValidElementType(*element))
    return emitError(loc)
           << "unsupported: cursor parameter element type " << *element;
  return Type(emitrust::SliceType::get(*element));
}

LogicalResult CImporter::bindCursorParam(const clang::ParmVarDecl *param,
                                         Value baseArg, Value cursorArg,
                                         Location paramLoc) {
  // The shared element-slice argument derefs once into the region base
  // place, exactly like a slice parameter's; reads render
  // `(*base)[i as usize]` and never hold a borrow across statements.
  // The element type rides in on the signature the caller built.
  auto sliceType = llvm::cast<emitrust::SliceType>(
      llvm::cast<emitrust::RefType>(baseArg.getType()).getPointee());
  Value basePlace =
      builder
          .create<emitrust::DerefOp>(
              paramLoc, emitrust::LValueType::get(sliceType), baseArg)
          .getResult();
  // The in-out cursor copies into a local i64 cell at entry; `*s` reads
  // and `*s = p` writes go through the cell, and every return site
  // copies it back through the reference (emitCursorWritebacks).
  IntegerType i64Type = builder.getIntegerType(64);
  Value cursorPlace =
      builder
          .create<emitrust::DerefOp>(
              paramLoc, emitrust::LValueType::get(i64Type), cursorArg)
          .getResult();
  Value initial =
      builder.create<emitrust::LoadOp>(paramLoc, i64Type, cursorPlace)
          .getResult();
  Value cell = createEntryAlloca(paramLoc, i64Type);
  builder.create<memref::StoreOp>(paramLoc, initial, cell);
  symbols[param] = basePlace;
  pointerLocals[param] = PointerLocalInfo{param, cell};
  cursorWritebacks.push_back({cell, cursorPlace});
  return success();
}

void CImporter::bindArgvParam(const clang::ParmVarDecl *param,
                              Value tableArg) {
  // C99-43 C3: record the `!emitrust.argv_table` argument for the
  // translation-time argv intercepts (printf `%s`/`%c` holes,
  // `argv[i][j]` byte places). Deliberately NOT entered into `symbols`:
  // every admitted use is intercepted syntactically, so an argv reference
  // reaching the generic decl-reference path fails loudly there instead
  // of silently borrowing a mistyped place.
  (void)param;
  mainArgvTableValue = tableArg;
}

void CImporter::emitCursorWritebacks(Location loc) {
  for (auto &[cell, place] : cursorWritebacks) {
    Value value = loadPlace(loc, cell);
    builder.create<emitrust::AssignOp>(loc, place, value);
  }
}

LogicalResult CImporter::emitVaClones(const clang::FunctionDecl *func,
                                      const VaMonomorphPlan &plan) {
  for (const VaClonePlan &clone : plan.clones)
    if (failed(emitVaClone(func, clone)))
      return failure();
  return success();
}

LogicalResult CImporter::emitVaClone(const clang::FunctionDecl *func,
                                     const VaClonePlan &clone) {
  Location loc = translateLoc(func->getLocation());

  // Signature: the named parameters keep their classified shapes, the
  // site's extras append as by-value parameters — no synthetic cursor
  // parameter; the consumption cursor is an internal local.
  ArrayRef<ParamKind> paramKinds = classifyPointerParams(func);
  SmallVector<Type> inputTypes;
  for (auto [index, param] : llvm::enumerate(func->parameters())) {
    if (cursorParams.contains(param)) {
      FailureOr<Type> sliceType = mapCursorParamSliceType(param);
      if (failed(sliceType))
        return failure();
      inputTypes.push_back(emitrust::RefType::get(*sliceType));
      inputTypes.push_back(
          emitrust::MutRefType::get(builder.getIntegerType(64)));
      continue;
    }
    if (pairedCursorParams.contains(param)) {
      inputTypes.push_back(
          emitrust::MutRefType::get(builder.getIntegerType(64)));
      continue;
    }
    if (globalCursorParams.contains(param)) {
      inputTypes.push_back(emitrust::MutRefType::get(optionCursorType()));
      continue;
    }
    FailureOr<Type> paramType =
        mapParamType(param->getType(), translateLoc(param->getLocation()),
                     paramKinds[index], voidByteSliceElem(func, index));
    if (failed(paramType))
      return failure();
    inputTypes.push_back(*paramType);
  }
  // (No FR-80 shared-const-struct flag here: a va-clone only exists for a
  // DEFINED variadic body, which the rule excludes twice over.)
  unsigned namedInputCount = inputTypes.size();
  for (Type extraType : clone.extraTypes)
    inputTypes.push_back(extraType);
  SmallVector<Type> resultTypes;
  clang::QualType returnType = func->getReturnType();
  if (!returnType->isVoidType()) {
    if (isDataPointer(returnType) || isFilePtrType(returnType))
      return emitError(loc)
             << "unsupported: pointer return from a variadic definition";
    FailureOr<Type> mapped = mapType(returnType, loc);
    if (failed(mapped))
      return failure();
    resultTypes.push_back(*mapped);
  }
  FunctionType functionType = builder.getFunctionType(inputTypes, resultTypes);
  if (functions.lookup(clone.name))
    return emitError(loc) // Defensive; the planner reserved the name.
           << "unsupported: monomorphization clone name '" << clone.name
           << "' collides with an existing symbol";

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(module.getBody());
  auto funcOp = builder.create<func::FuncOp>(loc, clone.name, functionType);
  functions[clone.name] = funcOp;

  // Function prologue: the same per-clone state reset importFunction
  // performs, with the va_start/va_arg/va_end lowerings armed.
  symbols.clear();
  addressTaken.clear();
  fileLocals.clear();
  pointerLocals.clear();
  stringViewLocals.clear();
  pointerPointerLocals.clear();
  carrierLocals.clear();
  carrierParams.clear();
  literalBackings.clear();
  paramCells.clear();
  ownerStructPlaces.clear();
  loopStack.clear();
  labelBlocks.clear();
  switchCaseBlocks.clear();
  cursorWritebacks.clear();
  pairedCursorPlaces.clear();
  globalCursorPlaces.clear();
  currentVaCloneActive = true;
  currentVaExtras.clear();
  currentHasLabels = containsLabelStmt(func->getBody());
  currentFunctionBody = func->getBody();
  placeBackedScalars.clear();
  inductionValues.clear();
  currentReceiverPlace = Value();
  currentPoolPlace = Value();
  currentMethodOwner = nullptr;
  currentOwnerIndexReturn = false;
  currentCxxThisRef = Value();
  currentReturnType = resultTypes.empty() ? Type() : resultTypes.front();
  // W2.24: a lifted lambda / va clone is never in the can-throw closure
  // and admits no try/catch; a throw inside one rejects loudly.
  currentFunctionThrows = false;
  currentThrowsOkType = Type();
  currentTryAllowed = false;
  tryContexts.clear();
  catchPayloadPlaces.clear();
  currentFamOptionPayload = famOptionReturnPayload(func, currentReturnType);
  famOptionTemps.clear();
  currentErasedReturnBase = nullptr;
  currentFuncName = clone.name;
  currentIsMain = false;
  bodyRegion = &funcOp.getBody();
  entryBlock = funcOp.addEntryBlock();
  builder.setInsertionPointToStart(entryBlock);
  collectAddressTaken(func->getBody());
  // FR-61f: mark body-touched scalars of range-eligible loops as places.
  collectRangeForPlaceScalars(func->getBody());
  pointerRegions.carrierReturnQuery =
      [this](const clang::FunctionDecl *callee) {
        return isCarrierReturnFunction(callee);
      };
  pointerRegions.ownerIndexReturnQuery =
      [this](const clang::FunctionDecl *callee) {
        return ownerIndexReturns.contains(callee->getCanonicalDecl());
      };
  // See the non-clone prologue above for why this mirrors
  // `ownerIndexReturnQuery`'s clone-path duplication (Stage 4, B3).
  pointerRegions.arrayMemberFieldQuery =
      [this](const clang::FieldDecl *field) {
        auto it = arrayMemberPtrBindings.find(field);
        return it != arrayMemberPtrBindings.end() &&
               it->second.invalidReason.empty();
      };
  collectVoidFnPtrHolders(func->getBody());
  pointerRegions.fnHolderQuery = [this](const clang::VarDecl *var) {
    return voidFnPtrHolders.contains(var);
  };
  pointerRegions.stringValueLocalQuery = [this](const clang::VarDecl *var) {
    return stringFillLocals.contains(var);
  };
  pointerRegions.vecValueLocalQuery = [this](const clang::VarDecl *var) {
    return vecValueLocals.contains(var);
  };
  // FR-94: same owned-tail bypass and FAM-leaf gate as the non-clone
  // prologue above.
  pointerRegions.famValueLocalQuery = [this](const clang::VarDecl *var) {
    return famAllocLocals.contains(var);
  };
  pointerRegions.famTailMemberQuery =
      [this](const clang::MemberExpr *member) {
        const auto *leaf =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
        return leaf && famTailField(leaf->getParent()) == leaf;
      };
  // FR-96: same member-read-local bypass and recognized-member-write
  // ownership as the non-clone prologue above.
  pointerRegions.famMemberLocalQuery = [this](const clang::VarDecl *var) {
    return famMemberLocals.contains(var);
  };
  pointerRegions.famMemberWriteQuery =
      [this, func](const clang::FieldDecl *field, const clang::Expr *rhs) {
        return famMemberLiftRecognizes(func, field, rhs);
      };
  // FR-93: same member-array decay classification as the non-clone
  // prologue above.
  pointerRegions.memberArrayDecayQuery =
      [this](const clang::MemberExpr *member) {
        return classifyMemberArrayDecay(member);
      };
  pointerRegions.literalTemps = &literalTemps;
  pointerRegions.cursorParamQuery = [this](const clang::ParmVarDecl *param) {
    return cursorParams.contains(param);
  };
  pointerRegions.cursorArgQuery = [this](const clang::FunctionDecl *callee,
                                         unsigned index) {
    const clang::FunctionDecl *definition = callee->getDefinition();
    if (!definition || index >= definition->getNumParams())
      return false;
    return cursorParams.contains(definition->getParamDecl(index));
  };
  // See the non-clone prologue above for the Shape-P join contract.
  pointerRegions.pairedArgQuery = [this](const clang::FunctionDecl *callee,
                                         unsigned index) -> int {
    const clang::FunctionDecl *definition = callee->getDefinition();
    if (!definition || index >= definition->getNumParams())
      return -1;
    const clang::ParmVarDecl *coParam =
        pairedCursorParams.lookup(definition->getParamDecl(index));
    if (!coParam)
      return -1;
    for (unsigned coIndex = 0; coIndex < definition->getNumParams();
         ++coIndex)
      if (definition->getParamDecl(coIndex) == coParam)
        return static_cast<int>(coIndex);
    return -1;
  };
  // Shape-G single-global-or-NULL positions (C99-43 C1): a `&p`
  // argument there is consumed by the Option-cell staging, binds the
  // plan's global as p's region base, and a null-writing callee marks
  // the region nullable.
  pointerRegions.globalCursorArgQuery =
      [this](const clang::FunctionDecl *callee,
             unsigned index) -> std::optional<GlobalCursorPlan> {
    const clang::FunctionDecl *definition = callee->getDefinition();
    if (!definition || index >= definition->getNumParams())
      return std::nullopt;
    auto it = globalCursorParams.find(definition->getParamDecl(index));
    if (it == globalCursorParams.end())
      return std::nullopt;
    return it->second;
  };
  pointerRegions.analyze(astContext(), func->getBody());

  // Named parameter binding, then the extras: the extra block arguments
  // stay raw SSA values (structs are Copy) selected by the va_arg
  // dispatch; the consumption cursor is an entry-block i64 cell.
  unsigned entryArgIndex = 0;
  for (const clang::ParmVarDecl *param : func->parameters()) {
    Location paramLoc = translateLoc(param->getLocation());
    if (cursorParams.contains(param)) {
      Value baseArg = entryBlock->getArgument(entryArgIndex);
      Value cursorArg = entryBlock->getArgument(entryArgIndex + 1);
      entryArgIndex += 2;
      if (failed(bindCursorParam(param, baseArg, cursorArg, paramLoc)))
        return failure();
      continue;
    }
    if (pairedCursorParams.contains(param)) {
      Value cursorArg = entryBlock->getArgument(entryArgIndex++);
      Value place = builder
                        .create<emitrust::DerefOp>(
                            paramLoc,
                            emitrust::LValueType::get(
                                builder.getIntegerType(64)),
                            cursorArg)
                        .getResult();
      pairedCursorPlaces[param] = place;
      continue;
    }
    if (globalCursorParams.contains(param)) {
      Value cellArg = entryBlock->getArgument(entryArgIndex++);
      Value place =
          builder
              .create<emitrust::DerefOp>(
                  paramLoc, emitrust::LValueType::get(optionCursorType()),
                  cellArg)
              .getResult();
      globalCursorPlaces[param] = place;
      continue;
    }
    Value blockArg = entryBlock->getArgument(entryArgIndex++);
    if (isDataPointer(param->getType()) &&
        blockArg.getType() == builder.getIntegerType(64))
      carrierParams.insert(param);
    if (failed(bindOrdinaryParam(param, blockArg, paramLoc)))
      return failure();
  }
  for (unsigned index = namedInputCount; index < inputTypes.size(); ++index)
    currentVaExtras.push_back(entryBlock->getArgument(index));
  currentVaCursorCell =
      createEntryAlloca(loc, builder.getIntegerType(64));

  if (failed(emitStmt(func->getBody())))
    return failure();
  return finalizeFunction(funcOp, loc);
}

FailureOr<Value> CImporter::emitVaArg(const clang::VAArgExpr *expr) {
  Location loc = translateLoc(expr->getBeginLoc());
  if (!currentVaCloneActive)
    return emitError(loc)
           << "unsupported: va_arg outside a variadic definition";
  FailureOr<Type> mapped = mapType(expr->getType(), loc);
  if (failed(mapped))
    return failure();
  Type type = *mapped;
  IntegerType i64Type = builder.getIntegerType(64);

  // Consume one position: read the cursor, then bump it.
  Value cursor = loadPlace(loc, currentVaCursorCell);
  Value one = createIntConstant(loc, i64Type, 1);
  Value next = builder.create<arith::AddIOp>(loc, cursor, one).getResult();
  builder.create<memref::StoreOp>(loc, next, currentVaCursorCell);

  // The dispatch selects among the extras whose static type is T. A
  // cursor position with no matching extra would be UB in the C call
  // (va_arg with the wrong type), so a deterministic panic is a legal
  // refinement.
  Value result = createVariablePlace(loc, type);
  SmallVector<unsigned, 4> candidates;
  for (auto [index, extra] : llvm::enumerate(currentVaExtras))
    if (extra.getType() == type)
      candidates.push_back(static_cast<unsigned>(index));
  auto emitPanic = [&]() {
    Attribute message = builder.getStringAttr(
        "va_arg: no fixed argument of the requested type");
    builder.create<emitrust::CallOpaqueOp>(
        loc, TypeRange(), builder.getStringAttr("panic!"),
        builder.getArrayAttr({message}), ValueRange());
  };
  if (candidates.empty()) {
    // No extra of this type exists in the clone at all (e.g. a
    // struct-typed va_arg inside a clone whose site passed only ints):
    // reaching this read at runtime is unconditionally UB in C.
    emitPanic();
    return loadPlace(loc, result);
  }
  Block *contBlock = createBlock();
  for (unsigned candidate : candidates) {
    Value expected = createIntConstant(loc, i64Type, candidate);
    Value matches = builder
                        .create<arith::CmpIOp>(loc, arith::CmpIPredicate::eq,
                                               cursor, expected)
                        .getResult();
    Block *matchBlock = createBlock();
    Block *nextBlock = createBlock();
    builder.create<cf::CondBranchOp>(loc, matches, matchBlock, ValueRange(),
                                     nextBlock, ValueRange());
    builder.setInsertionPointToEnd(matchBlock);
    builder.create<emitrust::AssignOp>(loc, result,
                                       currentVaExtras[candidate]);
    builder.create<cf::BranchOp>(loc, contBlock);
    builder.setInsertionPointToEnd(nextBlock);
  }
  emitPanic();
  builder.create<cf::BranchOp>(loc, contBlock);
  builder.setInsertionPointToEnd(contBlock);
  return loadPlace(loc, result);
}

LogicalResult CImporter::importDeclsIn(const clang::DeclContext *context) {
  for (const clang::Decl *decl : context->decls()) {
    if (decl->isImplicit())
      continue;
    // System-header declarations (angle-bracket includes, `-isystem`) are
    // skipped instead of imported eagerly: real libc headers are full of
    // constructs outside the supported subset (anonymous structs in
    // bits/types.h, variadic prototypes, ...), and a program that never
    // touches them must not be rejected for their sake. A main-file use of
    // a skipped declaration is rejected at the use site (see
    // `rejectSystemHeaderUse`); types are still imported on demand through
    // `mapType`. Project headers included via `-I` are not system headers
    // and keep the whole-file fail-fast import.
    if (isSystemHeaderDecl(decl))
      continue;
    // W2.0 C++ AST tolerance: `extern "C" { ... }` and `namespace { ... }`
    // are transparent containers for this dispatch — their nested members
    // import exactly as if written at this level (namespace members pick
    // up the flattening prefix through `namespacePrefix`, driven off each
    // declaration's own `DeclContext` rather than off this recursion, so
    // it composes automatically for nested namespaces).
    if (const auto *linkageSpec = llvm::dyn_cast<clang::LinkageSpecDecl>(decl)) {
      if (failed(importDeclsIn(linkageSpec)))
        return failure();
      continue;
    }
    if (const auto *ns = llvm::dyn_cast<clang::NamespaceDecl>(decl)) {
      if (failed(importDeclsIn(ns)))
        return failure();
      continue;
    }
    // Recoverable import (FR-42) wraps the per-item dispatch and nothing
    // else: the container recursions above are not items, and a rejection
    // inside one is recovered at the member that raised it. When recovery
    // is off this is the historical `failed(...) -> return failure()`.
    if (recoverFromRejections) {
      if (failed(importTopLevelDeclRecovering(decl)))
        return failure();
      continue;
    }
    if (failed(importTopLevelDecl(decl)))
      return failure();
  }
  return success();
}

/// W2.15: the located rejection a function-template specialization whose
/// template ARGUMENTS are outside the admitted subset earns, or success
/// when every argument is a plain type or (W2.28) an integral value.
///
/// Driven off the `TemplateArgument` kind rather than off anything in the
/// body, which is not a stylistic choice: a body-driven check is
/// measurably insufficient. `template <int N> int ident(int x) { return x; }`
/// never mentions `N`, and `template <typename... Ts> int first(int a, Ts...)`
/// need contain no pack expression — both bodies import as ordinary
/// functions, under a symbol whose suffix codes the non-type argument as
/// the `x` placeholder, so two instantiations silently fuse into one.
/// Both shapes are regression-pinned in
/// test/Import/Cpp/function-templates-invalid.cpp.
static LogicalResult checkTemplateArguments(const clang::FunctionDecl *spec,
                                            Location loc) {
  const clang::TemplateArgumentList *args =
      spec->getTemplateSpecializationArgs();
  if (!args)
    return success();
  for (const clang::TemplateArgument &arg : args->asArray()) {
    if (arg.getKind() == clang::TemplateArgument::Pack)
      return emitError(loc)
             << "unsupported: variadic function template (template "
                "parameter pack)";
    // W2.28: an INTEGRAL non-type argument is admitted — it codes by
    // value in the symbol suffix (`templateArgIntegralCode`), so two
    // instantiations cannot fuse. Every other non-type kind (declaration,
    // nullptr, template-template) still codes as the `x` placeholder and
    // stays rejected here.
    if (arg.getKind() != clang::TemplateArgument::Type &&
        arg.getKind() != clang::TemplateArgument::Integral)
      return emitError(loc) << "unsupported: non-type template argument in "
                               "function template instantiation";
  }
  return success();
}

LogicalResult CImporter::importTopLevelDecl(const clang::Decl *decl) {
  // W2.16 class-template monomorphization, the record twin of W2.15's
  // arm below. Clang has already instantiated every specialization the
  // program requests, each a concrete `ClassTemplateSpecializationDecl`
  // (which IS-A `CXXRecordDecl`) with substituted fields and instantiated
  // constructor/method members, so the walk hands them to W2.2's ordinary
  // `importRecord` + `importCXXMethods` surface UNCHANGED and never
  // touches the uninstantiated pattern (whose fields are dependent types
  // with no mapping). A never-instantiated template contributes no
  // specializations, so it is examined not at all.
  //
  // The frontier verdicts (explicit/partial specialization, non-type
  // argument, parameter pack) deliberately do NOT live here: a record
  // specialization is reached by THREE routes — this walk, the ordinary
  // top-level `RecordDecl` visit below, and on demand from `mapType` when
  // a local/member/parameter names the type — and only the last one is
  // still live once FR-42 recovery has dropped the first two. They live
  // on the record import itself (`importRecordUncached`), which all three
  // routes funnel through. See test/Import/Cpp/class-templates-invalid.cpp.
  //
  // KNOWN GAPS, recorded rather than left silent:
  //  * a MEMBER function template is declared inside the record, so it
  //    never reaches this walk and surfaces at its call as "call to
  //    unimported method".
  //  * a static DATA member of a class template is a global, not a field;
  //    the walk imports records and methods only, so a use surfaces as
  //    "reference to an unknown variable".
  //  * a record name carries no namespace prefix, so two same-named
  //    templates in different namespaces compose one spelling; that is a
  //    located collision rejection in `importRecordUncached`, never a
  //    silent merge.
  if (const auto *classTmpl = llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
    for (const clang::ClassTemplateSpecializationDecl *spec :
         classTmpl->specializations()) {
      if (!spec->isThisDeclarationADefinition())
        continue;
      // An implicit instantiation reports the PATTERN's location, so every
      // instantiation of one template shares a diagnostic location. That
      // is accepted (there is no better source position for generated
      // code) and is why the lit pins match line/column loosely.
      if (failed(importRecord(spec, translateLoc(spec->getLocation())))) {
        // FR-126 (channel 2, class arm): same not-reached attribution
        // as the function-template loop below; the failing spec's own row
        // is already ledgered by importRecord (FR-115), so only the
        // never-visited later siblings need rows. A sibling that a body
        // later demands on demand imports anyway and the report's
        // emission-wins guard keeps it Ported.
        std::string failedSym = graphItemSymbol(spec);
        if (rejectionLedger && !recoveryStubOnly && !failedSym.empty()) {
          bool after = false;
          for (const clang::ClassTemplateSpecializationDecl *later :
               classTmpl->specializations()) {
            if (later == spec) {
              after = true;
              continue;
            }
            if (!after || !later->isThisDeclarationADefinition())
              continue;
            std::string laterSym = graphItemSymbol(later);
            if (laterSym.empty())
              continue;
            Location laterLoc = translateLoc(later->getLocation());
            std::string message =
                (llvm::Twine("unsupported: specialization was not reached: "
                             "sibling specialization '") +
                 failedSym + "' of the same template was rejected first")
                    .str();
            rejectionLedger->record(emitrust::RejectedItem{
                laterSym, laterLoc, message,
                emitrust::classifyBlocker(message, laterLoc),
                /*stubbed=*/false, /*ownerSymbol=*/"", failedSym});
          }
        }
        return failure();
      }
    }
    return success();
  }
  // W2.15 function-template monomorphization. Clang has already
  // instantiated every specialization the program requests, each a
  // concrete `FunctionDecl` with a dependent-free body hanging off the
  // template; the importer walks THOSE and never touches the
  // uninstantiated pattern, whose parameters are dependent types with no
  // mapping. Skipping the pattern structurally (rather than filtering it
  // out) is also what keeps a NEVER-instantiated template — e.g. the
  // `template <size_t I> int get(...)` tuple-protocol overload in
  // test/Import/Cpp/cpp-structured-bindings.cpp — importing exactly as it
  // did before this wave: it contributes no specializations, so nothing
  // is examined and nothing is rejected.
  //
  // This arm runs BEFORE the `FunctionDecl` arm. A `FunctionTemplateDecl`
  // is not a `FunctionDecl`, so the order is not required by the type
  // dispatch, but the explicit-specialization guard below reads more
  // plainly next to the arm it pairs with.
  //
  // KNOWN GAPS, recorded rather than left silent:
  //  * an `extern template` explicit-instantiation DECLARATION is not a
  //    definition and is skipped here; a call to it surfaces at the call
  //    site as "call to unimported function '<sym>'".
  //  * a member function template never reaches this walk (it is declared
  //    inside a record) and surfaces as "call to unimported method".
  //  * two TUs instantiating one header template both emit the symbol and
  //    the FR-58 shard merge rejects with "duplicate definition of
  //    '<sym>' at link"; template vague linkage / COMDAT dedup is out of
  //    scope for W2.15.
  if (const auto *tmpl = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
    for (const clang::FunctionDecl *spec : tmpl->specializations()) {
      if (!spec->isThisDeclarationADefinition())
        continue;
      // An implicit instantiation reports the PATTERN's location, so every
      // instantiation of one template shares a diagnostic location. That
      // is accepted (there is no better source position for generated
      // code) and is why the lit pins match line/column loosely.
      Location specLoc = translateLoc(spec->getLocation());
      // W2.28: an EXPLICIT specialization is admitted through the very
      // same path as an implicit instantiation — it is a concrete
      // `FunctionDecl` whose body is the hand-written one, and clang
      // never also creates the implicit instantiation it displaces, so
      // it imports under the identical suffixed symbol with no collision
      // and no dispatch decision to make.
      // FR-126: FR-115-shape per-specialization capture, so a failed
      // sibling's own cause reaches the ledger under ITS symbol (the outer
      // recovery only records the TEMPLATE's name, off-graph).
      struct SpecCaptured {
        Location loc;
        DiagnosticSeverity severity;
        std::string message;
      };
      SmallVector<SpecCaptured> specCaptured;
      auto specCapture = [&specCaptured](Diagnostic &diag) -> LogicalResult {
        if (diag.getSeverity() != DiagnosticSeverity::Error)
          return failure();
        specCaptured.push_back(
            {diag.getLocation(), diag.getSeverity(), diag.str()});
        for (Diagnostic &note : diag.getNotes())
          specCaptured.push_back(
              {note.getLocation(), DiagnosticSeverity::Note, note.str()});
        return success();
      };
      LogicalResult specImported = failure();
      {
        ScopedDiagnosticHandler specHandler(builder.getContext(),
                                            specCapture);
        specImported = failed(checkTemplateArguments(spec, specLoc))
                           ? failure()
                           : importFunction(spec);
      }
      {
        std::optional<InFlightDiagnostic> active;
        for (const SpecCaptured &diag : specCaptured) {
          if (diag.severity == DiagnosticSeverity::Error) {
            if (active)
              active->report();
            active.emplace(emitError(diag.loc) << diag.message);
          } else if (active) {
            active->attachNote(diag.loc) << diag.message;
          }
        }
        if (active)
          active->report();
      }
      if (failed(specImported)) {
        // FR-126 (channel 2): the walk aborts on the first failed
        // sibling, so every LATER specialization of this template is never
        // visited and would read `unreached-by-import` with no root. Ledger
        // each one against the sibling that aborted the walk, so the report
        // can attribute through it.
        std::string failedSym = graphItemSymbol(spec);
        if (rejectionLedger && !recoveryStubOnly && !failedSym.empty()) {
          Location failLoc = specCaptured.empty()
                                 ? specLoc
                                 : specCaptured.front().loc;
          std::string failReason =
              specCaptured.empty() ? std::string("unsupported declaration")
                                   : specCaptured.front().message;
          rejectionLedger->record(emitrust::RejectedItem{
              failedSym, failLoc, failReason,
              emitrust::classifyBlocker(failReason, failLoc),
              /*stubbed=*/false, /*ownerSymbol=*/"",
              cascadeSourceForReason(failReason)});
          bool after = false;
          for (const clang::FunctionDecl *later : tmpl->specializations()) {
            if (later == spec) {
              after = true;
              continue;
            }
            if (!after || !later->isThisDeclarationADefinition())
              continue;
            std::string laterSym = graphItemSymbol(later);
            if (laterSym.empty())
              continue;
            Location laterLoc = translateLoc(later->getLocation());
            std::string message =
                (llvm::Twine("unsupported: specialization was not reached: "
                             "sibling specialization '") +
                 failedSym + "' of the same template was rejected first")
                    .str();
            rejectionLedger->record(emitrust::RejectedItem{
                laterSym, laterLoc, message,
                emitrust::classifyBlocker(message, laterLoc),
                /*stubbed=*/false, /*ownerSymbol=*/"", failedSym});
          }
        }
        return failure();
      }
    }
    return success();
  }
  if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    // W2.15/W2.28: an EXPLICIT SPECIALIZATION is visited twice — once
    // through its template's `specializations()` list above (which now
    // imports it), once here as an ordinary top-level declaration; like
    // any other specialization kind it must not import twice. Under
    // FR-42 recovery, dropping the template item leaves the spec simply
    // unimported and a call fails loudly at the call site ("call to
    // unimported function") — never a silent admission under the wrong
    // (unsuffixed) name, because this arm skips every specialization
    // kind.
    // An implicit or explicit INSTANTIATION belongs to the template arm
    // above, which already imported it; importing it again here would
    // collide with itself.
    if (func->getTemplateSpecializationKind() != clang::TSK_Undeclared)
      return success();
    return importFunction(func);
  }
  if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(decl)) {
    // W2.28: a partial-specialization PATTERN (`template <typename T>
    // struct W<T*> {...}`) is a dependent record with no concrete layout
    // — the exact analogue of the uninstantiated template pattern the
    // class-template arm above never touches, so it is skipped
    // STRUCTURALLY rather than rejected. Its INSTANTIATIONS are fully
    // concrete `ClassTemplateSpecializationDecl`s carrying the partial's
    // substituted body, reach `importRecord` through the walk above or on
    // demand from `mapType`, and import like any other specialization
    // (their template args are the PRIMARY template's concrete argument
    // list, so the W2.15/W2.16 suffix already names them uniquely —
    // `W<int*>` is `w_pi32`). Without this skip the pattern falls to
    // `importRecord`'s dependent-type guard and one never-used partial
    // aborts the whole TU.
    if (llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(record))
      return success();
    // W2.16: a `ClassTemplateSpecializationDecl` IS-A `RecordDecl` and
    // reaches HERE as well as through the class-template arm above (an
    // explicit specialization is listed in both places, and this visit is
    // the one still live once FR-42 recovery has dropped the template
    // item). No guard is needed at this site: `importRecord` is idempotent
    // for an already-imported specialization, and every frontier verdict
    // lives on the record import itself, so both visits agree.
    //
    // An EMPTY struct that no declaration type mentions is skipped: it
    // may only ever appear as a zero-byte member of a byte-region
    // aggregate (CTS-BR, 00216), which never materializes the record
    // type at all. One that IS declared with keeps the eager import.
    if (const clang::RecordDecl *definition = record->getDefinition();
        definition && definition->isStruct() && definition->field_empty() &&
        !declTypeUsedRecords.contains(definition))
      return success();
    return importRecord(record, translateLoc(record->getBeginLoc()));
  }
  if (const auto *enumDecl = llvm::dyn_cast<clang::EnumDecl>(decl))
    return importEnum(enumDecl, translateLoc(enumDecl->getBeginLoc()));
  // A `_Static_assert`/`static_assert` is a compile-time-only check the C/C++
  // frontend already evaluated; it produces no runtime code and is discarded,
  // like a typedef or an empty declaration.
  if (llvm::isa<clang::TypedefDecl>(decl) || llvm::isa<clang::EmptyDecl>(decl) ||
      llvm::isa<clang::StaticAssertDecl>(decl))
    return success();
  if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
    return importGlobalVar(var);
  return emitError(translateLoc(decl->getBeginLoc()))
         << "unsupported top-level declaration";
}

LogicalResult CImporter::importTranslationUnit(clang::ASTContext &context,
                                               llvm::StringRef tuTag,
                                               bool deferExtern,
                                               bool soleTranslationUnit) {
  astContextPtr = &context;
  currentTuTag = tuTag.str();
  deferExternGlobals = deferExtern;
  currentSoleTU = soleTranslationUnit;
  const clang::TranslationUnitDecl *unit = astContext().getTranslationUnitDecl();
  // FR-53: the Pass-A planners' per-declaration rejections are keyed by this
  // TU's own `clang::Decl *`s and consulted only by this TU's declaration
  // walk, so the map starts empty for every unit.
  plannerRejections.clear();
  pendingPlannerAttribution = nullptr;
  // Namespace pre-pass: record every module-symbol name this TU's ordinary
  // identifier namespace will claim, so struct tag naming
  // (`structSymbolName`) is independent of declaration order.
  collectOrdinaryNames(unit);
  // FR-101 Pass 0: borrow-bundle scalarization rewrites the clang AST
  // itself, so it must run before EVERY planner and every analysis — they
  // must all see the scalarized form and never the bundle. A no-op unless
  // the whole-TU gate holds (see ImportCBorrowBundle.cpp), which is what
  // keeps every emission that works today byte-for-byte identical.
  scalarizeBorrowBundles(context);
  // CTS-S Pass A: per-TU fn-ptr facts (written globals, address-taken
  // functions) and the devirtualization aliases of never-reassigned
  // global function pointers. Pure-AST; hoisted before `planOwners`
  // (FR-76) so the owner planner can consult `addressTakenFunctions` —
  // an address-taken function cannot become an owner method.
  planFnPtrAliases(unit);
  // W2.24 Pass A: the can-throw closure (exceptions as Result threading).
  // Pure-AST and provably inert for C (the walk never runs — C has no
  // throw); its located gate rejections must beat every import-time
  // diagnostic, and the closure it computes drives the carrier-enum
  // signature rewrite in `importFunction`.
  if (failed(planThrows(unit)))
    return failure();
  // Phase-4 Pass A: pure-AST owner planning over every function definition
  // before any IR is built; Pass B below consults the plans.
  planOwners(unit, soleTranslationUnit);
  // Stage 2 of the owner-struct self-reference extension: pure-AST,
  // consumes planOwners's output; strictly additive (see its doc comment).
  planArrayMemberPointers(unit);
  // W4.2e Part B (FR-39): the RFC index-handle node pool; pure-AST,
  // strictly additive, must run before `collectDeclTypeRecords` so a
  // promoted self-ref field's `Option<usize>` type reaches record emission.
  planMallocPool(unit);
  // FR-64: pure-AST recognition of constant-fill `char` buffers that lift to
  // an idiomatic `String::repeat`. Runs after `planMallocPool` (a string-fill
  // buffer is never a node pool) and before the pointer-region emission passes
  // consult `stringFillLocals` via `stringValueLocalQuery`.
  planStringFill(unit);
  // FR-65: pure-AST recognition of runtime-sized non-char scalar heap buffers
  // that lift to an owned `Vec<T>` (the Vec arm of the {array, Vec, span,
  // Option} representation match). Runs after `planStringFill` (char buffers
  // are the String/byte domain and are claimed first) and before the
  // pointer-region emission passes consult `vecValueLocals` via
  // `vecValueLocalQuery`.
  planVecLift(unit);
  // FR-94: pure-AST recognition of FAM-record owned-tail locals, owned-return
  // allocators, and free-only wrapper parameters. Runs after `planVecLift`
  // (its record-pointee candidates are disjoint from the scalar-buffer arms
  // above, but the arms stay strictly ordered) and before any signature is
  // built (`classifyPointerReturn`/`classifyPointerParams` consult the
  // owned-return and owned-parameter sets).
  planFamLift(unit);
  // CTS-P10 Pass A: cell-slice classification of pointer-parameter
  // classes whose bases are all mutable global arrays.
  planCellSlices(unit, soleTranslationUnit);
  // CTS 00204 Pass A: string-cursor parameter plans and va_list
  // monomorphization plans. Both run BEFORE any declaration imports so
  // their located rejections (escape shapes, va_copy, address-of) beat
  // the type rejections importing a callee prototype would raise.
  if (failed(planCursorParams(unit)))
    return failure();
  if (failed(planVaMonomorph(unit)))
    return failure();
  // CTS-BR (00216) Pass A: `void *` struct members whose every stored
  // value is the address of a function of one signature retype to
  // fn_ptr members; declaration-type record uses gate the eager import
  // of empty structs.
  planFnPtrMembers(unit);
  collectDeclTypeRecords(unit);
  if (failed(importDeclsIn(unit)))
    return failure();
  if (needsFloatFormatHelper && !floatFormatHelperEmitted) {
    floatFormatHelperEmitted = true;
    // C-compatible `%f` rendering: `{:.6}` matches C for finite values and
    // infinities, but Rust spells NaN as "NaN" where C prints "nan" with a
    // leading '-' when the sign bit is set. Emitted once per module, after
    // all imported items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_fmt_f64(x: f64) -> String {\n"
            "    if x.is_nan() {\n"
            "        if x.is_sign_negative() { String::from(\"-nan\") } "
            "else { String::from(\"nan\") }\n"
            "    } else {\n"
            "        format!(\"{:.6}\", x)\n"
            "    }\n"
            "}"));
  }
  if (needsCharFormatHelper && !charFormatHelperEmitted) {
    charFormatHelperEmitted = true;
    // C-compatible `%c`/putchar rendering: C converts the int argument to
    // unsigned char and writes that byte; `(x as u8) as char` emits the
    // identical byte for every ASCII value (0..=127). Values 128..=255
    // would render as two-byte UTF-8 and are documented as out of scope
    // (design.md C99-48). Emitted once per module, after all imported
    // items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr("fn __emitrust_fmt_c(x: i32) -> char {\n"
                                    "    (x as u8) as char\n"
                                    "}"));
  }
  if (needsCStrHelper && !cStrHelperEmitted) {
    cStrHelperEmitted = true;
    // C-compatible `%s` rendering of a char array: C prints bytes up to
    // (not including) the first NUL, which `take_while` mirrors; the
    // per-byte `u8 as char` conversion is exact for ASCII contents (the
    // importer rejects non-ASCII string data, design.md C99-47). Emitted
    // once per module, after all imported items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_cstr(s: &[i8]) -> String {\n"
            "    s.iter().take_while(|&&b| b != 0).map(|&b| (b as u8) as "
            "char).collect()\n"
            "}"));
  }
  if (needsCStrNHelper && !cStrNHelperEmitted) {
    cStrNHelperEmitted = true;
    // C-compatible `%.Ns` rendering of a char region: C writes at most N
    // bytes and stops earlier at a NUL; under a bounding precision the
    // region may legally lack a terminator (C99 7.19.6.1p8), which
    // `take(n)` mirrors by stopping at the slice end. ASCII-only like
    // `__emitrust_cstr`. Emitted once per module, after all imported
    // items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_cstr_n(s: &[i8], n: i64) -> String {\n"
            "    s.iter().take(n as usize).take_while(|&&b| b != 0)"
            ".map(|&b| (b as u8) as char).collect()\n"
            "}"));
  }
  if (needsCStrOutHelper && !cStrOutHelperEmitted) {
    cStrOutHelperEmitted = true;
    // C-compatible `%s` rendering of an argv argument (C99-43 C3): the
    // raw bytes up to (not including) the first NUL go straight to
    // stdout via `write_all`, BYPASSING the `__emitrust_cstr` Display
    // funnel whose Latin-1 byte-to-char widening would double-encode any
    // non-ASCII argument byte (the spike's matrix column C). The write
    // goes through the same globally buffered stdout handle `print!`
    // locks, so segment ordering holds even on block-buffered pipes.
    // Emitted once per module, after all imported items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_cstr_out(s: &[i8]) {\n"
            "    use std::io::Write;\n"
            "    let end = s.iter().position(|&b| b == 0)"
            ".unwrap_or(s.len());\n"
            "    let bytes: Vec<u8> = s[..end].iter().map(|&b| b as u8)"
            ".collect();\n"
            "    std::io::stdout().write_all(&bytes)"
            ".expect(\"stdout write failed\");\n"
            "}"));
  }
  if (needsCStrNOutHelper && !cStrNOutHelperEmitted) {
    cStrNOutHelperEmitted = true;
    // The `%.Ns` twin of `__emitrust_cstr_out`: at most N bytes, stopping
    // earlier at a NUL (C99 7.19.6.1p8 lets the run lack a terminator
    // when the precision bounds the read, which `take` mirrors by
    // stopping at the slice end). Raw bytes like the unbounded form.
    // Emitted once per module, after all imported items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_cstr_n_out(s: &[i8], n: i64) {\n"
            "    use std::io::Write;\n"
            "    let end = s.iter().take(n as usize)"
            ".position(|&b| b == 0)"
            ".unwrap_or(s.len().min(n as usize));\n"
            "    let bytes: Vec<u8> = s[..end].iter().map(|&b| b as u8)"
            ".collect();\n"
            "    std::io::stdout().write_all(&bytes)"
            ".expect(\"stdout write failed\");\n"
            "}"));
  }
  if (needsByteOutHelper && !byteOutHelperEmitted) {
    byteOutHelperEmitted = true;
    // C-compatible `%c` rendering of an argv byte (C99-43 C3): C
    // converts to unsigned char and writes that ONE byte; `write_all` of
    // the raw byte matches it for every value 0..=255, where the
    // `__emitrust_fmt_c` char widening would emit two-byte UTF-8 for
    // 128..=255. Shares the buffered stdout handle with `print!`.
    // Emitted once per module, after all imported items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_byte_out(b: i8) {\n"
            "    use std::io::Write;\n"
            "    std::io::stdout().write_all(&[b as u8])"
            ".expect(\"stdout write failed\");\n"
            "}"));
  }
  if (needsByteErrHelper && !byteErrHelperEmitted) {
    byteErrHelperEmitted = true;
    // W2.22: the stderr twin of `__emitrust_byte_out`, for a `char` operand
    // of a `std::cerr <<` chain. Identical body on the stderr handle, and
    // identical rationale: libstdc++ writes ONE raw byte for every
    // `char`/`signed char`/`unsigned char` value, where the
    // `__emitrust_fmt_c` char widening would emit two-byte UTF-8 for
    // 128..=255. Emitted once per module, after all imported items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_byte_err(b: i8) {\n"
            "    use std::io::Write;\n"
            "    std::io::stderr().write_all(&[b as u8])"
            ".expect(\"stderr write failed\");\n"
            "}"));
  }
  // FR-92: the on-demand `__emitrust_chunk_<R>x<C>` reshaping helpers,
  // one per (rows, cols) shape a 2D-array-pointer cast argument used
  // (`Cipher((state_t*)buf, ...)`): `as_chunks_mut::<C>()` views the
  // `&mut [u8]` byte run as C-byte rows and `try_into` fixes the row
  // count, panicking at runtime on an under-length view (the argv_arg
  // out-of-range precedent — panic where the C access was UB; a
  // STATICALLY undersized source already rejected at import). Each shape
  // is emitted once per module, after all imported items.
  for (std::pair<int64_t, int64_t> dims : neededChunkHelpers) {
    if (!emittedChunkHelpers.insert(dims).second)
      continue;
    std::string rows = std::to_string(dims.first);
    std::string cols = std::to_string(dims.second);
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_chunk_" + rows + "x" + cols +
            "(b: &mut [u8]) -> &mut [[u8; " + cols + "]; " + rows + "] {\n"
            "    let (rows, _) = b.as_chunks_mut::<" + cols + ">();\n"
            "    (&mut rows[.." + rows + "]).try_into().unwrap()\n"
            "}"));
  }
  if ((needsIntFormatSignedHelper || needsIntFormatUnsignedHelper) &&
      !intFormatCoreHelperEmitted) {
    intFormatCoreHelperEmitted = true;
    // C99 7.19.6.1 integer directive rendering, shared by the signed and
    // unsigned wrappers: precision zero-pads the digits (and a zero value
    // with precision zero prints nothing), '#' forces the leading octal
    // zero or the 0x/0X prefix, the sign/prefix sit inside the '0' width
    // padding, '0' is ignored next to '-' or a precision — all exactly
    // C's rules (validated byte-exactly against glibc). Flag bits:
    // '-'=1, '0'=2, '+'=4, ' '=8, '#'=16, uppercase=32.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_fmt_int(neg: bool, mag: u64, base: i32, "
            "prec: i32,\n"
            "                      width: i32, flags: i32) -> String {\n"
            "    let minus = flags & 1 != 0;\n"
            "    let zero = flags & 2 != 0 && prec < 0 && !minus;\n"
            "    let plus = flags & 4 != 0;\n"
            "    let space = flags & 8 != 0;\n"
            "    let alt = flags & 16 != 0;\n"
            "    let upper = flags & 32 != 0;\n"
            "    let mut digits = if mag == 0 && prec == 0 {\n"
            "        String::new()\n"
            "    } else if base == 8 {\n"
            "        format!(\"{:o}\", mag)\n"
            "    } else if base == 16 && upper {\n"
            "        format!(\"{:X}\", mag)\n"
            "    } else if base == 16 {\n"
            "        format!(\"{:x}\", mag)\n"
            "    } else {\n"
            "        format!(\"{}\", mag)\n"
            "    };\n"
            "    if prec > digits.len() as i32 {\n"
            "        digits = \"0\".repeat(prec as usize - digits.len()) "
            "+ &digits;\n"
            "    }\n"
            "    if alt && base == 8 && !digits.starts_with('0') {\n"
            "        digits.insert(0, '0');\n"
            "    }\n"
            "    let prefix = if alt && base == 16 && mag != 0 {\n"
            "        if upper { \"0X\" } else { \"0x\" }\n"
            "    } else {\n"
            "        \"\"\n"
            "    };\n"
            "    let sign = if neg { \"-\" } else if plus { \"+\" }\n"
            "               else if space { \" \" } else { \"\" };\n"
            "    let used = sign.len() + prefix.len() + digits.len();\n"
            "    let pad = if width > used as i32 { width as usize - used "
            "} else { 0 };\n"
            "    if minus {\n"
            "        format!(\"{}{}{}{}\", sign, prefix, digits, "
            "\" \".repeat(pad))\n"
            "    } else if zero {\n"
            "        format!(\"{}{}{}{}\", sign, prefix, "
            "\"0\".repeat(pad), digits)\n"
            "    } else {\n"
            "        format!(\"{}{}{}{}\", \" \".repeat(pad), sign, "
            "prefix, digits)\n"
            "    }\n"
            "}"));
  }
  if (needsIntFormatSignedHelper && !intFormatSignedHelperEmitted) {
    intFormatSignedHelperEmitted = true;
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_fmt_i64(x: i64, prec: i32, width: i32, "
            "flags: i32) -> String {\n"
            "    __emitrust_fmt_int(x < 0, x.unsigned_abs(), 10, prec, "
            "width, flags)\n"
            "}"));
  }
  if (needsIntFormatUnsignedHelper && !intFormatUnsignedHelperEmitted) {
    intFormatUnsignedHelperEmitted = true;
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_fmt_u64(x: u64, base: i32, prec: i32, "
            "width: i32,\n"
            "                      flags: i32) -> String {\n"
            "    __emitrust_fmt_int(false, x, base, prec, width, flags)\n"
            "}"));
  }
  if (needsFloatFormatExtHelper && !floatFormatExtHelperEmitted) {
    floatFormatExtHelperEmitted = true;
    // Exact C99 f/e/g floating rendering over Rust's correctly-rounded
    // decimal conversion ({:.*} and {:.*e} are exact for every finite
    // f64, and round half-to-even on the exact value like glibc).
    // `__emitrust_fmt_edigits` extracts correctly-rounded e-style digits
    // and reports whether rounding carried into the next decade (glibc's
    // %#g drops the mantissa fraction exactly when that carry lands on
    // ev == P; an exact power of ten keeps it). That carry-drop is a
    // glibc-only quirk — Darwin's libc keeps the zeros ("1.00000e+06"
    // where glibc prints "1.e+06") — so the emitted shim gates it on
    // cfg!(target_os = "linux"): the oracle is byte-parity with the HOST
    // C library the native reference binary links. Non-finite values pad
    // with spaces even under '0', as glibc does. Validated byte-exactly
    // against glibc across structured and fuzzed batteries. Flag bits as
    // in `__emitrust_fmt_int`; conv: 0=f, 1=e, 2=g.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_fmt_exp(m: &str, ev: i32, alt: bool, "
            "upper: bool) -> String {\n"
            "    let mut m = String::from(m);\n"
            "    if alt && !m.contains('.') {\n"
            "        m.push('.');\n"
            "    }\n"
            "    format!(\"{}{}{}{:02}\", m, if upper { 'E' } else "
            "{ 'e' },\n"
            "            if ev < 0 { '-' } else { '+' }, ev.abs())\n"
            "}\n"
            "\n"
            "fn __emitrust_fmt_edigits(mag: f64, prec: usize) -> "
            "(String, i32, bool) {\n"
            "    let s = format!(\"{:.*e}\", prec, mag);\n"
            "    let (m, e) = match s.split_once('e') {\n"
            "        Some(t) => t,\n"
            "        None => (s.as_str(), \"0\"),\n"
            "    };\n"
            // FR-109: `.unwrap_or(0)`, not a match and not
            // `.unwrap_or_default()`. The match spelling is what
            // clippy::manual_unwrap_or_default fires on -- 12 warnings
            // across 12 crates, the most replicated non-off-limits lint
            // in either corpus. `unwrap_or(0)` is the proven-clean
            // spelling: the `pre` binding four lines below already uses
            // it and has never warned.
            "    let ev: i32 = e.parse().unwrap_or(0);\n"
            "    let pre: i32 = {\n"
            "        let t = format!(\"{:e}\", mag);\n"
            "        match t.split_once('e') {\n"
            "            Some((_, te)) => te.parse().unwrap_or(0),\n"
            "            None => 0,\n"
            "        }\n"
            "    };\n"
            "    let int_len = m.find('.').unwrap_or(m.len());\n"
            "    if int_len == 1 {\n"
            "        return (String::from(m), ev, ev != pre);\n"
            "    }\n"
            "    let mut out = String::from(\"1\");\n"
            "    if prec > 0 {\n"
            "        out.push('.');\n"
            "        out.push_str(&\"0\".repeat(prec));\n"
            "    }\n"
            "    (out, ev + 1, true)\n"
            "}\n"
            "\n"
            "fn __emitrust_fmt_float(x: f64, conv: i32, prec: i32, "
            "width: i32,\n"
            "                        flags: i32) -> String {\n"
            "    let minus = flags & 1 != 0;\n"
            "    let zero = flags & 2 != 0 && !minus;\n"
            "    let plus = flags & 4 != 0;\n"
            "    let space = flags & 8 != 0;\n"
            "    let alt = flags & 16 != 0;\n"
            "    let upper = flags & 32 != 0;\n"
            "    let p = if prec < 0 { 6usize } else { prec as usize };\n"
            "    let sign = if x.is_sign_negative() { \"-\" } else if "
            "plus { \"+\" }\n"
            "               else if space { \" \" } else { \"\" };\n"
            "    let (body, numeric) = if x.is_nan() {\n"
            "        (String::from(if upper { \"NAN\" } else { \"nan\" })"
            ", false)\n"
            "    } else if x.is_infinite() {\n"
            "        (String::from(if upper { \"INF\" } else { \"inf\" })"
            ", false)\n"
            "    } else {\n"
            "        let mag = x.abs();\n"
            "        let text = if conv == 0 {\n"
            "            let mut t = format!(\"{:.*}\", p, mag);\n"
            "            if alt && p == 0 {\n"
            "                t.push('.');\n"
            "            }\n"
            "            t\n"
            "        } else if conv == 1 {\n"
            "            let (m, ev, _) = __emitrust_fmt_edigits(mag, p);\n"
            "            __emitrust_fmt_exp(&m, ev, alt, upper)\n"
            "        } else {\n"
            "            let pp = if p == 0 { 1 } else { p };\n"
            "            let (m, ev, carried) = "
            "__emitrust_fmt_edigits(mag, pp - 1);\n"
            "            if ev < -4 || ev >= pp as i32 {\n"
            "                let mut m = if carried && ev == pp as i32\n"
            "                    && cfg!(target_os = \"linux\") {\n"
            "                    String::from(\"1\")\n"
            "                } else {\n"
            "                    m\n"
            "                };\n"
            "                if !alt && m.contains('.') {\n"
            "                    m = String::from(\n"
            "                        m.trim_end_matches('0')"
            ".trim_end_matches('.'));\n"
            "                }\n"
            "                __emitrust_fmt_exp(&m, ev, alt, upper)\n"
            "            } else {\n"
            "                let digits: String =\n"
            "                    m.chars().filter(|c| *c != '.')"
            ".collect();\n"
            "                let mut t = if ev >= 0 {\n"
            "                    let ip = ev as usize + 1;\n"
            "                    if digits.len() > ip {\n"
            "                        format!(\"{}.{}\", &digits[..ip], "
            "&digits[ip..])\n"
            "                    } else {\n"
            "                        String::from(&digits[..ip])\n"
            "                    }\n"
            "                } else {\n"
            "                    format!(\"0.{}{}\", "
            "\"0\".repeat((-ev - 1) as usize), digits)\n"
            "                };\n"
            "                if !alt && t.contains('.') {\n"
            "                    t = String::from(\n"
            "                        t.trim_end_matches('0')"
            ".trim_end_matches('.'));\n"
            "                }\n"
            "                if alt && !t.contains('.') {\n"
            "                    t.push('.');\n"
            "                }\n"
            "                t\n"
            "            }\n"
            "        };\n"
            "        (text, true)\n"
            "    };\n"
            "    let used = sign.len() + body.len();\n"
            "    let pad = if width > used as i32 { width as usize - used "
            "} else { 0 };\n"
            "    if minus {\n"
            "        format!(\"{}{}{}\", sign, body, \" \".repeat(pad))\n"
            "    } else if zero && numeric {\n"
            "        format!(\"{}{}{}\", sign, \"0\".repeat(pad), body)\n"
            "    } else {\n"
            "        format!(\"{}{}{}\", \" \".repeat(pad), sign, body)\n"
            "    }\n"
            "}"));
  }
  if (needsSprintfHelper && !sprintfHelperEmitted) {
    sprintfHelperEmitted = true;
    // C-compatible sprintf tail: copies the formatted ASCII bytes plus the
    // terminating NUL into the destination char region and returns the
    // written length (excluding the NUL), C's sprintf result. Every write
    // is a bounds-checked slice index, so a destination too small for the
    // bytes plus the NUL panics — C leaves that overflow undefined, and
    // the deterministic panic is a legal refinement. Emitted once per
    // module, after all imported items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_sprintf(dest: &mut [i8], s: &str) -> i32 {\n"
            "    let bytes = s.as_bytes();\n"
            "    for (i, &b) in bytes.iter().enumerate() {\n"
            "        dest[i] = b as i8;\n"
            "    }\n"
            "    dest[bytes.len()] = 0;\n"
            "    bytes.len() as i32\n"
            "}"));
  }
  if (needsSnprintfHelper && !snprintfHelperEmitted) {
    snprintfHelperEmitted = true;
    // C-compatible snprintf tail: writes at most `size - 1` formatted ASCII
    // bytes plus a terminating NUL into the destination char region (C's
    // DEFINED truncation, unlike sprintf's overflow), and returns the full
    // formatted length excluding the NUL — the value C's snprintf returns
    // regardless of truncation. `size == 0` writes nothing. Every write is a
    // bounds-checked slice index, so a `size` larger than the destination
    // region (a genuine C buffer overflow, undefined) panics rather than
    // writing out of bounds. Emitted once per module, after all imported
    // items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_snprintf(dest: &mut [i8], size: i64, s: &str) -> "
            "i32 {\n"
            "    let bytes = s.as_bytes();\n"
            "    if size > 0 {\n"
            "        let cap = (size as usize) - 1;\n"
            "        let n = if bytes.len() < cap { bytes.len() } else { cap "
            "};\n"
            "        for i in 0..n {\n"
            "            dest[i] = bytes[i] as i8;\n"
            "        }\n"
            "        dest[n] = 0;\n"
            "    }\n"
            "    bytes.len() as i32\n"
            "}"));
  }
  // Hosted <string.h> helpers (design.md C99-48, CTS-L1): each requested
  // helper is emitted once per module, in this fixed order, as safe Rust
  // over i8 slices. Every access is a bounds-checked slice index — the
  // borrowed regions are compile-time-sized char arrays (or literal
  // backings, which always end in a NUL) — so a C program whose behavior
  // is undefined (a missing terminator, an out-of-range count) panics
  // instead of reading out of bounds. Comparisons compare as unsigned
  // char, exactly C's rule.
  static const struct {
    llvm::StringRef name;
    llvm::StringRef source;
  } kStringHelpers[] = {
      {"__emitrust_strcpy",
       "fn __emitrust_strcpy(dst: &mut [i8], src: &[i8]) {\n"
       "    let mut i = 0usize;\n"
       "    loop {\n"
       "        let b = src[i];\n"
       "        dst[i] = b;\n"
       "        if b == 0 { break; }\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_strncpy",
       "fn __emitrust_strncpy(dst: &mut [i8], src: &[i8], n: i64) {\n"
       "    let mut ended = false;\n"
       "    let mut i = 0usize;\n"
       "    while (i as i64) < n {\n"
       "        let b = if ended { 0 } else { src[i] };\n"
       "        if b == 0 { ended = true; }\n"
       "        dst[i] = b;\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_strcat",
       "fn __emitrust_strcat(dst: &mut [i8], src: &[i8]) {\n"
       "    let mut d = 0usize;\n"
       "    while dst[d] != 0 { d += 1; }\n"
       "    let mut i = 0usize;\n"
       "    loop {\n"
       "        let b = src[i];\n"
       "        dst[d + i] = b;\n"
       "        if b == 0 { break; }\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_strcmp",
       "fn __emitrust_strcmp(a: &[i8], b: &[i8]) -> i32 {\n"
       "    let mut i = 0usize;\n"
       "    loop {\n"
       "        let x = a[i] as u8;\n"
       "        let y = b[i] as u8;\n"
       "        if x != y || x == 0 { return (x as i32) - (y as i32); }\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_strncmp",
       "fn __emitrust_strncmp(a: &[i8], b: &[i8], n: i64) -> i32 {\n"
       "    let mut i = 0usize;\n"
       "    while (i as i64) < n {\n"
       "        let x = a[i] as u8;\n"
       "        let y = b[i] as u8;\n"
       "        if x != y || x == 0 { return (x as i32) - (y as i32); }\n"
       "        i += 1;\n"
       "    }\n"
       "    0\n"
       "}"},
      {"__emitrust_strchr",
       "fn __emitrust_strchr(s: &[i8], c: i32) -> i64 {\n"
       "    let c = c as u8 as i8;\n"
       "    let mut i = 0usize;\n"
       "    loop {\n"
       "        if s[i] == c { return i as i64; }\n"
       "        if s[i] == 0 { return -1; }\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_strrchr",
       "fn __emitrust_strrchr(s: &[i8], c: i32) -> i64 {\n"
       "    let c = c as u8 as i8;\n"
       "    let mut last: i64 = -1;\n"
       "    let mut i = 0usize;\n"
       "    loop {\n"
       "        if s[i] == c { last = i as i64; }\n"
       "        if s[i] == 0 { return last; }\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_memset",
       "fn __emitrust_memset(s: &mut [i8], c: i32, n: i64) {\n"
       "    let b = c as u8 as i8;\n"
       "    let mut i = 0usize;\n"
       "    while (i as i64) < n {\n"
       "        s[i] = b;\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_memcpy",
       "fn __emitrust_memcpy(dst: &mut [i8], src: &[i8], n: i64) {\n"
       "    let mut i = 0usize;\n"
       "    while (i as i64) < n {\n"
       "        dst[i] = src[i];\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_memcpy_within",
       "fn __emitrust_memcpy_within(s: &mut [i8], dst: i64, src: i64, n: "
       "i64) {\n"
       "    s.copy_within(src as usize..(src + n) as usize, dst as usize);\n"
       "}"},
      {"__emitrust_atoi",
       // C's atoi (7.20.1.2): skip isspace bytes (space and 0x09..0x0D),
       // one optional sign, then decimal digits to the first non-digit;
       // no digits yields 0. The accumulator counts downward so INT_MIN
       // parses exactly, and out-of-range values — C UB (7.20.1p1) — are
       // refined to deterministic i32 wrapping. A region with neither a
       // NUL nor a non-digit before its end simply stops at the end
       // (reading past the array is C UB, refined).
       "fn __emitrust_atoi(s: &[i8]) -> i32 {\n"
       "    let mut i = 0usize;\n"
       "    while i < s.len() {\n"
       "        let b = s[i] as u8;\n"
       "        if b != b' ' && (b < 9 || b > 13) { break; }\n"
       "        i += 1;\n"
       "    }\n"
       "    let mut neg = false;\n"
       "    if i < s.len() {\n"
       "        let b = s[i] as u8;\n"
       "        if b == b'+' || b == b'-' {\n"
       "            neg = b == b'-';\n"
       "            i += 1;\n"
       "        }\n"
       "    }\n"
       "    let mut acc: i32 = 0;\n"
       "    while i < s.len() {\n"
       "        let b = s[i] as u8;\n"
       "        if b < b'0' || b > b'9' { break; }\n"
       "        acc = acc.wrapping_mul(10).wrapping_sub((b - b'0') as "
       "i32);\n"
       "        i += 1;\n"
       "    }\n"
       "    if neg { acc } else { acc.wrapping_neg() }\n"
       "}"},
      {"__emitrust_memcmp",
       "fn __emitrust_memcmp(a: &[i8], b: &[i8], n: i64) -> i32 {\n"
       "    let mut i = 0usize;\n"
       "    while (i as i64) < n {\n"
       "        let x = a[i] as u8;\n"
       "        let y = b[i] as u8;\n"
       "        if x != y { return (x as i32) - (y as i32); }\n"
       "        i += 1;\n"
       "    }\n"
       "    0\n"
       "}"},
      // FR-72: the u8 images of the byte-family helpers, selected when
      // the call site's regions are ui8 (uint8_t/unsigned char slice
      // parameters, including FR-71-admitted void*). Byte-for-byte the
      // same algorithms as the i8 originals — mem* is sign-agnostic and
      // both compare/fill in the unsigned domain — only the slice
      // element differs, keeping the helper signature in agreement with
      // the call site.
      {"__emitrust_memset_u8",
       "fn __emitrust_memset_u8(s: &mut [u8], c: i32, n: i64) {\n"
       "    let b = c as u8;\n"
       "    let mut i = 0usize;\n"
       "    while (i as i64) < n {\n"
       "        s[i] = b;\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_memcpy_u8",
       "fn __emitrust_memcpy_u8(dst: &mut [u8], src: &[u8], n: i64) {\n"
       "    let mut i = 0usize;\n"
       "    while (i as i64) < n {\n"
       "        dst[i] = src[i];\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_memcmp_u8",
       "fn __emitrust_memcmp_u8(a: &[u8], b: &[u8], n: i64) -> i32 {\n"
       "    let mut i = 0usize;\n"
       "    while (i as i64) < n {\n"
       "        let x = a[i];\n"
       "        let y = b[i];\n"
       "        if x != y { return (x as i32) - (y as i32); }\n"
       "        i += 1;\n"
       "    }\n"
       "    0\n"
       "}"},
      // FR-87: the u8 image of the same-region copy_within helper, for
      // same-(root, field-path) memcpy/memmove over a ui8 member array
      // (memmove's overlap-correct semantics, refining C's undefined
      // overlapping memcpy exactly like the i8 original).
      {"__emitrust_memcpy_within_u8",
       "fn __emitrust_memcpy_within_u8(s: &mut [u8], dst: i64, src: i64, "
       "n: i64) {\n"
       "    s.copy_within(src as usize..(src + n) as usize, dst as usize);\n"
       "}"},
      // FR-93: the split primitive of the same-base (mut, shared) call
      // pair — the aes cbc `XorWithIv(buf, Iv)` arm once Iv walks buf's
      // region. `split_at_mut` at the MUTABLE cursor keeps both borrows
      // legal; the shared window then subscripts strictly below it, so
      // an out-of-window read panics where the C read reached into the
      // mutable half (the accepted loud refinement of C UB-adjacent
      // reads; never a silent wrong byte).
      {"__emitrust_split_mut_u8",
       "fn __emitrust_split_mut_u8(s: &mut [u8], n: i64) -> (&mut [u8], "
       "&mut [u8]) {\n"
       "    s.split_at_mut(n as usize)\n"
       "}"},
      // FR-87: the word-fill memset image for a `unsigned int` member
      // array destination. `n` stays the BYTE count; admission requires
      // it to be a constant multiple of 4 and the fill word to be the
      // constant replicated fill byte (b * 0x01010101, endianness-
      // neutral since all four bytes are equal), so the word walk is
      // byte-exact memset.
      {"__emitrust_memset_u32",
       "fn __emitrust_memset_u32(s: &mut [u32], w: u32, n: i64) {\n"
       "    let mut i = 0usize;\n"
       "    while ((i * 4) as i64) < n {\n"
       "        s[i] = w;\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      // FR-97: the per-width siblings of the FR-87 u32 word-fill image —
      // byte-splat memset over TYPED integer arrays (i16/u16/i32/i64/u64
      // elements, LOCAL or member destinations). `n` stays the BYTE
      // count and the walker strides the element size, so the same
      // admission gates (constant count, multiple of the element size;
      // constant replicated fill word) keep every fill byte-exact.
      {"__emitrust_memset_i16",
       "fn __emitrust_memset_i16(s: &mut [i16], w: i16, n: i64) {\n"
       "    let mut i = 0usize;\n"
       "    while ((i * 2) as i64) < n {\n"
       "        s[i] = w;\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_memset_u16",
       "fn __emitrust_memset_u16(s: &mut [u16], w: u16, n: i64) {\n"
       "    let mut i = 0usize;\n"
       "    while ((i * 2) as i64) < n {\n"
       "        s[i] = w;\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_memset_i32",
       "fn __emitrust_memset_i32(s: &mut [i32], w: i32, n: i64) {\n"
       "    let mut i = 0usize;\n"
       "    while ((i * 4) as i64) < n {\n"
       "        s[i] = w;\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_memset_i64",
       "fn __emitrust_memset_i64(s: &mut [i64], w: i64, n: i64) {\n"
       "    let mut i = 0usize;\n"
       "    while ((i * 8) as i64) < n {\n"
       "        s[i] = w;\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
      {"__emitrust_memset_u64",
       "fn __emitrust_memset_u64(s: &mut [u64], w: u64, n: i64) {\n"
       "    let mut i = 0usize;\n"
       "    while ((i * 8) as i64) < n {\n"
       "        s[i] = w;\n"
       "        i += 1;\n"
       "    }\n"
       "}"},
  };
  for (const auto &helper : kStringHelpers) {
    if (!neededStringHelpers.contains(helper.name) ||
        emittedStringHelpers.contains(helper.name))
      continue;
    emittedStringHelpers.insert(helper.name);
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(helper.source));
  }
  if (needsStrlenHelper && !strlenHelperEmitted) {
    strlenHelperEmitted = true;
    // C-compatible strlen over a string-literal region: counts bytes up to
    // (not including) the first NUL. The backing of every string-literal
    // region includes the terminating NUL, so `position` always finds one;
    // the `unwrap_or` fallback merely keeps the helper total. Emitted once
    // per module, after all imported items.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "fn __emitrust_strlen(s: &[i8]) -> i64 {\n"
            "    s.iter().position(|&b| b == 0).unwrap_or(s.len()) as i64\n"
            "}"));
  }
  // Hosted <stdio.h> FILE* helpers (design.md C99-48, CTS-T1.3): the
  // owned-handle enum plus one safe-Rust helper per lowered stream
  // operation, each requested by `requestFileHelper` and emitted once per
  // module in this fixed order (the enum first; fread/fgets pull in the
  // fgetc primitive they call). fopen failure is C's NULL path
  // (`__EmitrustFile::Null`); every use of a Null or wrong-direction
  // handle, an I/O error beyond end-of-file, and an out-of-range count
  // is C undefined behavior refined into a deterministic panic (the
  // slice indexing is bounds-checked).
  static const struct {
    llvm::StringRef name;
    llvm::StringRef source;
  } kFileHelpers[] = {
      {"__EmitrustFile",
       "/// An owned C `FILE*` stream handle over std::fs (design.md\n"
       "/// C99-48). `Null` is C's NULL: a failed fopen, or the state\n"
       "/// fclose leaves so the same variable can be reopened. Streams\n"
       "/// are sequential-only and byte-wise; using a Null or\n"
       "/// wrong-direction handle is C UB, refined into a deterministic\n"
       "/// panic by the helpers below.\n"
       "enum __EmitrustFile {\n"
       "    Null,\n"
       "    Read(std::fs::File),\n"
       "    Write(std::fs::File),\n"
       "}"},
      {"__emitrust_fopen_r",
       "/// `fopen(path, \"r\")`: opens an existing file for sequential\n"
       "/// reading; `Null` is C's NULL result when the open fails.\n"
       "fn __emitrust_fopen_r(path: &str) -> __EmitrustFile {\n"
       "    match std::fs::File::open(path) {\n"
       "        Ok(f) => __EmitrustFile::Read(f),\n"
       "        Err(_) => __EmitrustFile::Null,\n"
       "    }\n"
       "}"},
      {"__emitrust_fopen_w",
       "/// `fopen(path, \"w\")`: creates or truncates a file for\n"
       "/// sequential writing; `Null` is C's NULL result when the open\n"
       "/// fails.\n"
       "fn __emitrust_fopen_w(path: &str) -> __EmitrustFile {\n"
       "    match std::fs::File::create(path) {\n"
       "        Ok(f) => __EmitrustFile::Write(f),\n"
       "        Err(_) => __EmitrustFile::Null,\n"
       "    }\n"
       "}"},
      {"__emitrust_file_ok",
       "/// The truth of a C `FILE*` handle: false exactly when it is\n"
       "/// NULL (the `if (!f)` check after fopen).\n"
       "fn __emitrust_file_ok(f: &__EmitrustFile) -> bool {\n"
       "    !matches!(f, __EmitrustFile::Null)\n"
       "}"},
      {"__emitrust_fgetc",
       "/// `fgetc`/`getc`: one byte as 0..=255, or -1 (C's EOF) at end\n"
       "/// of file. Reading a NULL or write-mode stream is C UB, and an\n"
       "/// I/O error beyond end-of-file has no C-visible result either;\n"
       "/// both panic deterministically.\n"
       "fn __emitrust_fgetc(f: &mut __EmitrustFile) -> i32 {\n"
       "    match f {\n"
       "        __EmitrustFile::Read(file) => {\n"
       "            let mut byte = [0u8; 1];\n"
       "            match std::io::Read::read(file, &mut byte) {\n"
       "                Ok(0) => -1,\n"
       "                Ok(_) => byte[0] as i32,\n"
       "                Err(e) => panic!(\"fgetc: {}\", e),\n"
       "            }\n"
       "        }\n"
       "        _ => panic!(\"fgetc on a stream not open for reading\"),\n"
       "    }\n"
       "}"},
      {"__emitrust_fread",
       "/// Byte-wise `fread(ptr, 1, n, f)`: reads up to `n` bytes into\n"
       "/// the destination's prefix and returns the count actually read\n"
       "/// (short at end of file). A count exceeding the destination\n"
       "/// panics on the bounds-checked index (C UB, refined).\n"
       "fn __emitrust_fread(f: &mut __EmitrustFile, buf: &mut [i8], n: i64) "
       "-> i64 {\n"
       "    let mut count = 0i64;\n"
       "    while count < n {\n"
       "        let c = __emitrust_fgetc(f);\n"
       "        if c < 0 {\n"
       "            break;\n"
       "        }\n"
       "        buf[count as usize] = c as i8;\n"
       "        count += 1;\n"
       "    }\n"
       "    count\n"
       "}"},
      {"__emitrust_fwrite",
       "/// Byte-wise `fwrite(ptr, 1, n, f)`: writes the source's first\n"
       "/// `n` bytes and returns `n`, C's full-success result. A write\n"
       "/// error, a stream not open for writing, and a count exceeding\n"
       "/// the source all panic deterministically (C UB, refined).\n"
       "fn __emitrust_fwrite(f: &mut __EmitrustFile, buf: &[i8], n: i64) "
       "-> i64 {\n"
       "    match f {\n"
       "        __EmitrustFile::Write(file) => {\n"
       "            let bytes: Vec<u8> =\n"
       "                buf[..n as usize].iter().map(|&b| b as u8).collect();\n"
       "            std::io::Write::write_all(file, &bytes)\n"
       "                .expect(\"fwrite failed\");\n"
       "            n\n"
       "        }\n"
       "        _ => panic!(\"fwrite on a stream not open for writing\"),\n"
       "    }\n"
       "}"},
      {"__emitrust_fgets",
       "/// `fgets(buf, size, f)`: reads at most `size - 1` bytes,\n"
       "/// stopping after a newline, and NUL-terminates what was read.\n"
       "/// Returns -1 for C's NULL result (end of file with nothing\n"
       "/// read), else the count of bytes stored before the NUL.\n"
       "fn __emitrust_fgets(f: &mut __EmitrustFile, buf: &mut [i8], n: i64) "
       "-> i64 {\n"
       "    if n < 1 {\n"
       "        return -1;\n"
       "    }\n"
       "    let mut i = 0i64;\n"
       "    while i + 1 < n {\n"
       "        let c = __emitrust_fgetc(f);\n"
       "        if c < 0 {\n"
       "            break;\n"
       "        }\n"
       "        buf[i as usize] = c as i8;\n"
       "        i += 1;\n"
       "        if c == 10 {\n"
       "            break;\n"
       "        }\n"
       "    }\n"
       "    if i == 0 && n > 1 {\n"
       "        return -1;\n"
       "    }\n"
       "    buf[i as usize] = 0;\n"
       "    i\n"
       "}"},
      {"__emitrust_fclose",
       "/// `fclose(f)`: drops the handle (closing the file) and leaves\n"
       "/// the variable NULL, so the same C variable can be reopened by\n"
       "/// a later fopen (the 00187 serial-reuse shape). Closing NULL is\n"
       "/// C UB; leaving it NULL is a benign refinement.\n"
       "fn __emitrust_fclose(f: &mut __EmitrustFile) {\n"
       "    *f = __EmitrustFile::Null;\n"
       "}"},
  };
  for (const auto &helper : kFileHelpers) {
    if (!neededFileHelpers.contains(helper.name) ||
        emittedFileHelpers.contains(helper.name))
      continue;
    emittedFileHelpers.insert(helper.name);
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(helper.source));
  }
  // W4.2e Part B (FR-39): the Option<usize> pool-handle bridge helpers,
  // emitted once when any node-pool field is promoted. `_opt` builds the
  // nullable field value from a handle's (non-null, index) pair; `_unpack`
  // destructures a field read back into that pair.
  if (neededPoolHelpers && !poolHelpersEmitted) {
    poolHelpersEmitted = true;
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "/// W4.2e Part B: build a node-pool index field (`Option<usize>`)\n"
            "/// from a handle's non-null flag and i64 index (design.md FR-39).\n"
            "fn __emitrust_pool_opt(some: bool, idx: i64) -> Option<usize> {\n"
            "    if some { Some(idx as usize) } else { None }\n"
            "}"));
    moduleBuilder.create<emitrust::VerbatimOp>(
        UnknownLoc::get(builder.getContext()),
        moduleBuilder.getStringAttr(
            "/// W4.2e Part B: destructure a node-pool index field into a\n"
            "/// handle's (non-null, index) pair (design.md FR-39).\n"
            "fn __emitrust_pool_unpack(o: Option<usize>) -> (bool, i64) {\n"
            "    match o { Some(x) => (true, x as i64), None => (false, 0) }\n"
            "}"));
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Function-body plumbing
//===----------------------------------------------------------------------===//

Value CImporter::createEntryAlloca(Location loc, Type elementType) {
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(entryBlock);
  auto memrefType = MemRefType::get({}, elementType);
  return builder.create<memref::AllocaOp>(loc, memrefType).getResult();
}

Block *CImporter::createBlock() {
  OpBuilder::InsertionGuard guard(builder);
  return builder.createBlock(bodyRegion, bodyRegion->end());
}

Block *CImporter::getLabelBlock(const clang::LabelDecl *label) {
  Block *&block = labelBlocks[label];
  if (!block)
    block = createBlock();
  return block;
}

/// FR-99: the payload struct `S` of a nullable owned FAM return's
/// `Option<S>` signature, or a null Type when `func` has no such return.
/// Gated on `famNullableReturnFns` so no other Option-typed signature (the
/// C++ `std::optional` returns) can be mistaken for one.
Type CImporter::famOptionReturnPayload(const clang::FunctionDecl *func,
                                       Type returnType) {
  if (!func || !famNullableReturnFns.contains(func->getCanonicalDecl()))
    return Type();
  auto opaque = llvm::dyn_cast_or_null<emitrust::OpaqueType>(returnType);
  if (!opaque)
    return Type();
  llvm::StringRef payload = opaque.getValue();
  if (!payload.consume_front("Option<") || !payload.consume_back(">") ||
      payload.empty())
    return Type();
  return emitrust::StructType::get(builder.getContext(), payload);
}

Value CImporter::createVariablePlace(Location loc, Type type,
                                     llvm::StringRef rustName, Attribute init) {
  OpBuilder::InsertionGuard guard(builder);
  if (currentHasLabels)
    builder.setInsertionPointToStart(entryBlock);
  return builder
      .create<emitrust::VariableOp>(loc, emitrust::LValueType::get(type), init,
                                    /*isConst=*/false, rustName)
      .getResult();
}

bool CImporter::isTerminated(Block *block) {
  return !block->empty() && block->back().hasTrait<OpTrait::IsTerminator>();
}

Value CImporter::createIntConstant(Location loc, Type type, int64_t value) {
  return builder
      .create<arith::ConstantOp>(loc, builder.getIntegerAttr(type, value))
      .getResult();
}

Value CImporter::createBoolConstant(Location loc, bool value) {
  return builder.create<arith::ConstantOp>(loc, builder.getBoolAttr(value))
      .getResult();
}

Value CImporter::createScalarIntConstant(Location loc, Type type,
                                         int64_t value) {
  if (isUnsignedInt(type))
    return builder
        .create<emitrust::ConstantOp>(loc, type,
                                      IntegerAttr::get(type, value))
        .getResult();
  return createIntConstant(loc, type, value);
}

LogicalResult CImporter::finalizeFunction(func::FuncOp funcOp, Location loc) {
  Region &region = funcOp.getBody();

  // Erase blocks unreachable from the entry block (dead code after returns
  // and empty merge blocks). Cross-block SSA uses only ever reference
  // entry-block values, so dropping the dead blocks' defs and references
  // first makes erasure safe in any order.
  llvm::SmallPtrSet<Block *, 16> reachable;
  SmallVector<Block *> worklist{&region.front()};
  while (!worklist.empty()) {
    Block *block = worklist.pop_back_val();
    if (!reachable.insert(block).second)
      continue;
    for (Block *successor : block->getSuccessors())
      worklist.push_back(successor);
  }
  for (Block &block : region) {
    if (reachable.contains(&block))
      continue;
    block.dropAllDefinedValueUses();
    block.dropAllReferences();
  }
  for (Block &block : llvm::make_early_inc_range(region))
    if (!reachable.contains(&block))
      block.erase();

  // Sweep write-only parameter cells: a prologue cell whose every
  // remaining use is a store belongs to a parameter that is never read on
  // any surviving path (e.g. its only uses folded away with a statically
  // null pointer, CTS-P9). Dropping the stores and the cell is exact —
  // the stored values' computations stay behind as pure ops — and keeps
  // fully folded functions free of runtime state.
  for (Value cell : paramCells) {
    Operation *alloca = cell.getDefiningOp();
    if (!alloca)
      continue;
    SmallVector<Operation *> users(cell.getUsers().begin(),
                                   cell.getUsers().end());
    if (!llvm::all_of(users, [](Operation *user) {
          return llvm::isa<memref::StoreOp>(user);
        }))
      continue;
    for (Operation *user : users)
      user->erase();
    alloca->erase();
  }

  // Terminate the fall-through block, if any.
  for (Block &block : region) {
    if (isTerminated(&block))
      continue;
    builder.setInsertionPointToEnd(&block);
    if (!currentReturnType) {
      emitCursorWritebacks(loc);
      builder.create<func::ReturnOp>(loc);
      continue;
    }
    // C11 5.1.2.2.3: falling off the end of main returns 0. For any other
    // non-void function, C11 6.9.1p12 leaves the behavior defined as long
    // as the caller never uses the missing value; the importer synthesizes
    // a `return 0` of the function's return type (Rust has no
    // fall-off-the-end for value-returning functions, and the zero is only
    // observable on executions that were undefined reads in C anyway).
    if (llvm::isa<IntegerType>(currentReturnType)) {
      Value zero = createScalarIntConstant(loc, currentReturnType, 0);
      emitCursorWritebacks(loc);
      builder.create<func::ReturnOp>(loc, zero);
      continue;
    }
    if (auto floatType = llvm::dyn_cast<FloatType>(currentReturnType)) {
      Value zero = builder
                       .create<arith::ConstantOp>(
                           loc, FloatAttr::get(floatType, 0.0))
                       .getResult();
      emitCursorWritebacks(loc);
      builder.create<func::ReturnOp>(loc, zero);
      continue;
    }
    // Aggregate, enum, and fn_ptr returns have no meaningful zero.
    return emitError(loc)
           << "unsupported: control reaches the end of non-void function '"
           << funcOp.getSymName() << "'";
  }
  return success();
}

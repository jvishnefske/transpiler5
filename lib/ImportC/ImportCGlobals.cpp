//===- ImportCGlobals.cpp - file-scope global import -----------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// CImporter's file-scope global import: importGlobalVar/importPointerGlobal/
/// deferExternGlobal/createGlobal, the convertGlobalInit/convertAPValueInit/
/// convertRecordAPValue/convertAnonymousSlotInit constant-initializer
/// conversion family, and the global-writeback bookkeeping
/// (lookupGlobal/asDirectGlobalRef/rootsAtGlobal/flushGlobalWriteback/
/// commitGlobalWriteback). Split out of ImportC.cpp by pure code motion
/// (W1.8); see CImporterInternal.h for the CImporter class declaration this
/// file implements.
//
//===----------------------------------------------------------------------===//

#include "CImporterInternal.h"

using namespace mlir;

LogicalResult CImporter::importGlobalVar(const clang::VarDecl *var) {
  Location loc = translateLoc(var->getLocation());
  const clang::VarDecl *canonical = var->getCanonicalDecl();
  if (globals.contains(canonical))
    return success(); // Redeclaration of an already imported global.
  // A devirtualized function-pointer alias (CTS-S, 00189) materializes no
  // global at all; every use lowers against its target.
  if (fnPtrAliases.contains(canonical))
    return success();

  if (var->getTLSKind() != clang::VarDecl::TLS_None)
    return emitError(loc) << "unsupported: thread-local global variable";

  // Internal-linkage (`static`) globals are mangled with the per-TU tag so
  // identically named file-statics in different TUs stay distinct; external
  // globals keep their bare C name and unify across TUs. The tag is empty for
  // a single-TU import, preserving the historical bare name.
  bool internal = !var->isExternallyVisible();
  std::string symbolName =
      internal ? currentTuTag + var->getName().str() : var->getName().str();

  // C reconciliation of redeclarations: a variable that is only ever
  // `extern`-declared has no storage in this translation unit; a tentative
  // definition (`int g;`) behaves as a zero-initialized definition.
  if (var->hasDefinition() == clang::VarDecl::DeclarationOnly) {
    // Referenced-only import of main-file extern declarations (the same
    // policy system-header declarations follow, C99-39): an extern object
    // that nothing in this TU references demands no storage anywhere and
    // imports nothing.
    if (!var->isReferenced())
      return success();
    if (!deferExternGlobals)
      return emitError(loc) << "unsupported: extern global variable without a "
                               "definition in this translation unit";
    // Project import: another TU may define this external symbol. Defer the
    // existence check to `finalizeProject` after every TU is merged.
    return deferExternGlobal(canonical, symbolName, var->getType(), loc);
  }

  // The declaration carrying the initializer (if any) supplies the type;
  // otherwise the most recent declaration does, whose type is the merged
  // composite of all redeclarations.
  const clang::VarDecl *initDecl = nullptr;
  const clang::Expr *init = canonical->getAnyInitializer(initDecl);
  const clang::VarDecl *typeDecl =
      init ? initDecl : canonical->getMostRecentDecl();
  clang::QualType varType = typeDecl->getType().getCanonicalType();
  // An owned stream handle has no global model (C99-48): the check must
  // precede the pointer-global cursor decomposition below.
  if (isFilePtrType(varType))
    return emitError(loc)
           << "unsupported: FILE* is only supported as a function-local "
              "variable";
  if (varType->isPointerType() && !varType->isFunctionPointerType())
    return importPointerGlobal(canonical, typeDecl, symbolName, loc);
  return createGlobal(canonical, typeDecl, symbolName, loc);
}

LogicalResult CImporter::importPointerGlobal(const clang::VarDecl *key,
                                             const clang::VarDecl *decl,
                                             llvm::StringRef symbolName,
                                             Location loc) {
  // Referenced-only import: an unreferenced pointer global demands no
  // storage anywhere in the supported subset (nothing can observe it), so
  // declarations like `struct S *s;` — even with incomplete pointee
  // types — import nothing.
  if (!key->isReferenced())
    return success();
  // Program-wide facts are merged per TU by `planOwners`; an externally
  // visible pointer global in a multi-file project could be rebound by a
  // TU whose facts are not visible when this one imports.
  if (!currentSoleTU && key->isExternallyVisible())
    return emitError(loc) << "unsupported: pointer-typed global variable "
                             "with external linkage in a multi-file project";
  if (isRustKeyword(symbolName))
    return emitError(loc) << "unsupported: global variable name '"
                          << symbolName << "' is a Rust keyword";
  if (symbolName == "__emitrust_tl")
    return emitError(loc) << "unsupported: global variable name "
                             "'__emitrust_tl' is reserved for the "
                             "thread-local accessor binder";
  auto checkFreshSymbol = [&](llvm::StringRef name) -> LogicalResult {
    if (SymbolTable::lookupSymbolIn(module, name))
      return emitError(loc) << "unsupported: global variable '" << name
                            << "' collides with an existing symbol";
    return success();
  };

  clang::QualType pointee =
      decl->getType().getCanonicalType()->getPointeeType();

  // Start from the program-wide body facts and merge the file-scope
  // initializer's binding: static storage requires a constant initializer,
  // so clang's evaluator yields an lvalue APValue — a base (declaration or
  // compound literal) plus a byte offset.
  PointerRegion facts = globalPtrFacts.lookup(key);
  const clang::CompoundLiteralExpr *literalInit = nullptr;
  const clang::StringLiteral *stringInit = nullptr;
  int64_t initByteOffset = 0;
  Location initLoc = loc;
  // Converts the initializer's byte offset into the flat cursor unit: the
  // number of innermost (non-array) elements of `objectType` it spans.
  // Fails (nullopt) when the offset does not land on an element boundary.
  auto flatCursorOffset =
      [&](clang::QualType objectType) -> std::optional<int64_t> {
    clang::QualType innermost = astContext().getBaseElementType(objectType);
    int64_t innerBytes =
        astContext().getTypeSizeInChars(innermost).getQuantity();
    if (innerBytes <= 0 || initByteOffset < 0 ||
        initByteOffset % innerBytes != 0)
      return std::nullopt;
    return initByteOffset / innerBytes;
  };
  if (const clang::Expr *init = decl->getInit()) {
    initLoc = translateLoc(init->getBeginLoc());
    const clang::APValue *value = decl->evaluateValue();
    if (!value || !value->isLValue())
      return emitError(initLoc) << "unsupported: global pointer initializer";
    if (value->isNullPointer())
      return emitError(initLoc) << "unsupported: null pointer constant "
                                   "assigned to a pointer variable";
    initByteOffset = value->getLValueOffset().getQuantity();
    clang::APValue::LValueBase lvalueBase = value->getLValueBase();
    if (const auto *baseDecl =
            lvalueBase.dyn_cast<const clang::ValueDecl *>()) {
      const auto *baseVar = llvm::dyn_cast<clang::VarDecl>(baseDecl);
      if (!baseVar || baseVar->hasLocalStorage())
        return emitError(initLoc)
               << "unsupported: global pointer initializer";
      PointerRegion initBinding;
      initBinding.bases.push_back(PointerBaseBinding{
          baseVar->getCanonicalDecl(), init->getBeginLoc()});
      mergeRegionFacts(facts, initBinding);
    } else if (const auto *baseExpr =
                   lvalueBase.dyn_cast<const clang::Expr *>()) {
      literalInit = llvm::dyn_cast<clang::CompoundLiteralExpr>(baseExpr);
      stringInit = llvm::dyn_cast<clang::StringLiteral>(baseExpr);
      if (!literalInit && !stringInit)
        return emitError(initLoc)
               << "unsupported: global pointer initializer";
    } else {
      return emitError(initLoc) << "unsupported: global pointer initializer";
    }
  }

  // Region validation, mirroring `emitPointerLocal`: the first
  // invalidating construct (a binding to a local object, a copied global
  // pointer, an escaping address, ...) rejects at its own site.
  if (!facts.invalidReason.empty())
    return emitError(translateLoc(facts.invalidLoc)) << facts.invalidReason;
  if (facts.literalBase)
    return emitError(translateLoc(facts.literalLoc))
           << "unsupported: global pointer bound to a string literal";
  unsigned baseKinds = (facts.bases.empty() ? 0 : 1) +
                       (facts.allocSite ? 1 : 0) + (literalInit ? 1 : 0) +
                       (stringInit ? 1 : 0);
  if (baseKinds > 1 || facts.bases.size() >= 2) {
    if (facts.bases.size() >= 2) {
      const PointerBaseBinding &first = facts.bases[0];
      const PointerBaseBinding &second = facts.bases[1];
      InFlightDiagnostic diag = emitError(loc);
      diag << "unsupported: global pointer '" << symbolName
           << "' would join objects '" << first.base->getName() << "' and '"
           << second.base->getName() << "' into one region";
      diag.attachNote(translateLoc(first.loc))
          << "bound to '" << first.base->getName() << "' here";
      diag.attachNote(translateLoc(second.loc))
          << "bound to '" << second.base->getName() << "' here";
      return diag;
    }
    return emitError(loc) << "unsupported: global pointer '" << symbolName
                          << "' bound to multiple objects";
  }
  if (baseKinds == 0)
    return emitError(loc) << "unsupported: global pointer variable '"
                          << symbolName << "' has no known target object";
  // Member-rooted bases (CTS-P9) are a pointer-local shape: the stored
  // global cursor scheme has no member projection to store.
  if (!facts.bases.empty() && facts.bases.front().member)
    return emitError(translateLoc(facts.bases.front().loc))
           << "unsupported: global pointer bound to a struct member";

  OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
  IntegerType i64Type = builder.getIntegerType(64);
  auto createCursorGlobal = [&](llvm::StringRef name,
                                int64_t start) -> LogicalResult {
    if (failed(checkFreshSymbol(name)))
      return failure();
    moduleBuilder.create<emitrust::GlobalOp>(
        loc, moduleBuilder.getStringAttr(name), TypeAttr::get(i64Type),
        moduleBuilder.getIntegerAttr(i64Type, start), UnitAttr());
    return success();
  };

  // Shape 1: a promoted constant-size allocation — a zero-initialized
  // backing array global plus the cursor global.
  if (facts.allocSite) {
    Location allocLoc = translateLoc(facts.allocLoc);
    FailureOr<Type> elementType = mapType(pointee, allocLoc);
    if (failed(elementType))
      return failure();
    Type backingType = emitrust::ArrayType::get(
        builder.getContext(), facts.allocCount, *elementType);
    std::string backingName = (symbolName + "_backing").str();
    if (failed(checkFreshSymbol(backingName)))
      return failure();
    moduleBuilder.create<emitrust::GlobalOp>(
        loc, moduleBuilder.getStringAttr(backingName),
        TypeAttr::get(backingType), Attribute(), UnitAttr());
    if (failed(createCursorGlobal(symbolName, 0)))
      return failure();
    pointerGlobals[key] =
        PointerGlobalInfo{key, backingName, backingType, symbolName.str()};
    return success();
  }

  // Shape 2: a file-scope compound literal — a synthesized
  // constant-initialized backing global; degenerate for scalar/struct
  // literals, cursor-carrying for array literals.
  if (literalInit) {
    Location initLoc = translateLoc(literalInit->getBeginLoc());
    clang::QualType literalType = literalInit->getType();
    FailureOr<Type> backingType = mapType(literalType, initLoc);
    if (failed(backingType))
      return failure();
    clang::Expr::EvalResult literalValue;
    if (!literalInit->getInitializer()->EvaluateAsRValue(literalValue,
                                                         astContext()) ||
        literalValue.HasSideEffects)
      return emitError(initLoc)
             << "unsupported: non-constant global initializer";
    FailureOr<Attribute> init =
        convertAPValueInit(literalValue.Val, *backingType, literalType,
                           initLoc);
    if (failed(init))
      return failure();
    // Data-pointer members of the backing bind through the pointer
    // global's own key (the backing is 1:1 with the pointer), so
    // `s->f`-style member reads resolve statically (CTS-P2).
    collectGlobalMemberBindings(key, literalValue.Val, literalType,
                                literalInit->getBeginLoc());
    std::string backingName = (symbolName + "_backing").str();
    if (failed(checkFreshSymbol(backingName)))
      return failure();
    moduleBuilder.create<emitrust::GlobalOp>(
        loc, moduleBuilder.getStringAttr(backingName),
        TypeAttr::get(*backingType), *init, UnitAttr());
    if (astContext().getAsConstantArrayType(literalType)) {
      std::optional<int64_t> start = flatCursorOffset(literalType);
      if (!start)
        return emitError(initLoc)
               << "unsupported: global pointer initializer";
      if (failed(createCursorGlobal(symbolName, *start)))
        return failure();
      pointerGlobals[key] = PointerGlobalInfo{key, backingName, *backingType,
                                              symbolName.str()};
      return success();
    }
    if (facts.hasArithmetic)
      return emitError(translateLoc(facts.arithmeticLoc))
             << "unsupported: arithmetic on the address of a scalar object";
    if (initByteOffset != 0 ||
        !astContext().hasSameUnqualifiedType(pointee, literalType))
      return emitError(initLoc)
             << "unsupported: pointer type does not match its target object";
    pointerGlobals[key] =
        PointerGlobalInfo{key, backingName, *backingType, std::string()};
    return success();
  }

  // Shape 2b: a file-scope string-literal initializer (`char *s = "...";`,
  // CTS-L3) — the CTS-P1 read-only literal backing lifted to module scope.
  // The literal's bytes plus the terminating NUL become an immutable
  // `<name>_backing` byte-array global (never written: any write through
  // the region is rejected below, since writing a C string literal is UB),
  // and the pointer becomes a stored i64 cursor global into it.
  if (stringInit) {
    Location bindLoc = translateLoc(stringInit->getBeginLoc());
    if (facts.hasWriteThrough)
      return emitError(translateLoc(facts.writeThroughLoc))
             << "unsupported: write through a pointer to a string literal "
                "(the literal is read-only)";
    // Nullable literal regions are outside the CTS-P8 scope, matching the
    // function-local literal-region policy.
    if (facts.nullable)
      return emitError(translateLoc(facts.nullableLoc))
             << "unsupported: null pointer constant assigned to a pointer "
                "into a string literal";
    if (!stringInit->isOrdinary())
      return emitError(bindLoc) << "unsupported: non-ordinary string "
                                   "literal bound to a pointer";
    FailureOr<Type> elementType = mapType(pointee, bindLoc);
    if (failed(elementType))
      return failure();
    Type byteType = builder.getIntegerType(8);
    if (*elementType != byteType)
      return emitError(bindLoc)
             << "unsupported: pointer element type does not match its "
                "string literal";
    // The backing holds the bytes plus the terminating NUL, so a
    // strlen-style walk terminates inside the array. Non-ASCII bytes are
    // rejected so the region's contents stay exact through the ASCII-only
    // `%s`/`%c` printing helpers (the CTS-P1/C99-28 policy).
    uint64_t length = stringInit->getLength();
    SmallVector<Attribute> bytes;
    bytes.reserve(length + 1);
    for (uint64_t i = 0; i != length; ++i) {
      uint32_t byte = stringInit->getCodeUnit(i);
      if (byte > 127)
        return emitError(bindLoc) << "unsupported: non-ASCII byte in "
                                     "string literal bound to a pointer";
      bytes.push_back(
          IntegerAttr::get(byteType, static_cast<int64_t>(byte)));
    }
    bytes.push_back(IntegerAttr::get(byteType, 0));
    // The initializer's byte offset is the flat cursor directly (i8
    // elements); anything past one-past-the-end is not a constant C
    // pointer value, so the bound is defensive.
    if (initByteOffset < 0 ||
        static_cast<uint64_t>(initByteOffset) > length + 1)
      return emitError(initLoc) << "unsupported: global pointer initializer";
    auto backingType = emitrust::ArrayType::get(builder.getContext(),
                                                length + 1, byteType);
    std::string backingName = (symbolName + "_backing").str();
    if (failed(checkFreshSymbol(backingName)))
      return failure();
    moduleBuilder.create<emitrust::GlobalOp>(
        loc, moduleBuilder.getStringAttr(backingName),
        TypeAttr::get(backingType), builder.getArrayAttr(bytes),
        moduleBuilder.getUnitAttr());
    if (failed(createCursorGlobal(symbolName, initByteOffset)))
      return failure();
    pointerGlobals[key] =
        PointerGlobalInfo{key, backingName, backingType, symbolName.str()};
    return success();
  }

  // Shape 3: a real global object base. The base's own `emitrust.global`
  // is resolved at each access (it may be declared later in the TU); the
  // type compatibility check runs on the C types, mirroring
  // `emitPointerLocal`.
  const PointerBaseBinding &binding = facts.bases.front();
  const clang::VarDecl *base = binding.base;
  Location bindLoc = translateLoc(binding.loc);
  if (base->hasLocalStorage()) // Defensive; `addBase` rejects this first.
    return emitError(bindLoc)
           << "unsupported: global pointer bound to local object '"
           << base->getName() << "' (the borrow would outlive the object)";
  if (const clang::ConstantArrayType *array =
          astContext().getAsConstantArrayType(base->getType())) {
    bool matchesLevel = false;
    for (const clang::ConstantArrayType *level = array; level;
         level = astContext().getAsConstantArrayType(
             level->getElementType())) {
      if (astContext().hasSameUnqualifiedType(pointee,
                                              level->getElementType())) {
        matchesLevel = true;
        break;
      }
    }
    if (!matchesLevel)
      return emitError(bindLoc)
             << "unsupported: pointer element type does not match its "
                "target array";
    std::optional<int64_t> start = flatCursorOffset(base->getType());
    if (!start)
      return emitError(initLoc) << "unsupported: global pointer initializer";
    if (failed(createCursorGlobal(symbolName, *start)))
      return failure();
    pointerGlobals[key] =
        PointerGlobalInfo{base, std::string(), Type(), symbolName.str()};
    return success();
  }
  if (isPointerType(base->getType())) // Defensive; `addBase` forbids it.
    return emitError(bindLoc)
           << "unsupported: global pointer bound to a pointer object";
  if (facts.hasArithmetic)
    return emitError(translateLoc(facts.arithmeticLoc))
           << "unsupported: arithmetic on the address of a scalar object";
  if (initByteOffset != 0 ||
      !astContext().hasSameUnqualifiedType(pointee, base->getType()))
    return emitError(bindLoc)
           << "unsupported: pointer type does not match its target object";
  pointerGlobals[key] =
      PointerGlobalInfo{base, std::string(), Type(), std::string()};
  return success();
}

LogicalResult CImporter::deferExternGlobal(const clang::VarDecl *key,
                                           llvm::StringRef symbolName,
                                           clang::QualType qualType,
                                           Location loc) {
  if (qualType.getCanonicalType()->isPointerType() &&
      !qualType.getCanonicalType()->isFunctionPointerType())
    return emitError(loc) << "unsupported: pointer-typed global variable";
  FailureOr<Type> mlirType = mapType(qualType, loc);
  if (failed(mlirType))
    return failure();
  globals[key] = GlobalInfo{symbolName.str(), *mlirType};
  pendingExternGlobals.try_emplace(symbolName, loc);
  return success();
}

LogicalResult CImporter::createGlobal(const clang::VarDecl *key,
                                      const clang::VarDecl *decl,
                                      llvm::StringRef symbolName,
                                      Location loc) {
  if (symbolName.empty())
    return emitError(loc) << "unsupported: unnamed global variable";
  if (isRustKeyword(symbolName))
    return emitError(loc) << "unsupported: global variable name '"
                          << symbolName << "' is a Rust keyword";
  // Globals are emitted as `static` items (thread-local or plain), and Rust
  // identifier patterns cannot shadow statics, so a global spelled like the
  // thread-local accessor binder would break every mutable-global access.
  if (symbolName == "__emitrust_tl")
    return emitError(loc) << "unsupported: global variable name "
                             "'__emitrust_tl' is reserved for the "
                             "thread-local accessor binder";
  if (Operation *existing = SymbolTable::lookupSymbolIn(module, symbolName)) {
    auto existingGlobal = llvm::dyn_cast<emitrust::GlobalOp>(existing);
    if (!deferExternGlobals || !existingGlobal)
      return emitError(loc) << "unsupported: global variable '" << symbolName
                            << "' collides with an existing symbol";
    // Project import: a second file-scope definition of the same external
    // global. A tentative definition (no initializer) yields to a real one;
    // two real definitions are a duplicate-definition error.
    bool incomingHasInit = decl->getInit() != nullptr;
    if (!incomingHasInit) {
      globals[key] = GlobalInfo{symbolName.str(), existingGlobal.getType()};
      return success();
    }
    if (existingGlobal.getInitAttr())
      return emitError(loc)
             << "unsupported: conflicting definition of global variable '"
             << symbolName
             << "' (already defined in another translation unit)";
    existingGlobal.erase(); // Upgrade the tentative definition to this one.
  }

  clang::QualType qualType = decl->getType();
  // C99-7: scan the whole global's type (pointer globals bypass
  // `mapType`, and `int * volatile g` carries the qualifier on the
  // pointer itself).
  if (hasVolatileQualifier(astContext(), qualType))
    return emitError(loc) << "unsupported: volatile-qualified type";
  // Function pointers map to `!emitrust.fn_ptr` and are legal globals;
  // data pointers stay rejected.
  if (qualType.getCanonicalType()->isPointerType() &&
      !qualType.getCanonicalType()->isFunctionPointerType())
    return emitError(loc) << "unsupported: pointer-typed global variable";
  // CTS-BR (00216): a byte-region aggregate global is a byte-image
  // global — computed against the target layout, zero-filled, and
  // extended past sizeof by a static flexible-array-member tail.
  if (isByteRegionAggregate(qualType))
    return createByteRegionGlobal(key, decl, symbolName, loc);
  FailureOr<Type> mlirType = mapType(qualType, loc);
  if (failed(mlirType))
    return failure();

  Attribute initAttr;
  if (decl->getInit()) {
    FailureOr<Attribute> converted = convertGlobalInit(decl, *mlirType, loc);
    if (failed(converted))
      return failure();
    initAttr = *converted;
    // Record the static member bindings of any data-pointer fields the
    // initialized aggregate carries (CTS-P2); the stored i64 members
    // themselves converted to 0 above.
    if (const clang::APValue *value = decl->evaluateValue())
      collectGlobalMemberBindings(key, *value, decl->getType(),
                                  decl->getInit()->getBeginLoc());
  }

  // A const-qualified global is never written (clang rejects writes), so it
  // becomes an immutable Rust static. Struct- and fn_ptr-typed const
  // globals keep the mutable (Cell) representation: the GlobalOp `const`
  // marker is limited to scalar and array value types.
  bool isConst =
      qualType.isConstQualified() &&
      !llvm::isa<emitrust::StructType, emitrust::FnPtrType>(*mlirType);

  OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
  moduleBuilder.create<emitrust::GlobalOp>(
      loc, moduleBuilder.getStringAttr(symbolName), TypeAttr::get(*mlirType),
      initAttr, isConst ? moduleBuilder.getUnitAttr() : UnitAttr());
  globals[key] = GlobalInfo{symbolName.str(), *mlirType};
  return success();
}

FailureOr<Attribute> CImporter::convertGlobalInit(const clang::VarDecl *decl,
                                                  Type type, Location loc) {
  const clang::Expr *init = decl->getInit();
  Location initLoc = init ? translateLoc(init->getBeginLoc()) : loc;
  // A file-scope `char s[] = "..."` folds to a plain i8 element list
  // through the APValue path below, but non-ASCII bytes are rejected up
  // front (mirroring the block-scope string initializer) so the array's
  // contents stay exact through the ASCII-only `%s`/`%c` printing helpers.
  // A wide literal's code units fold to i32 elements that never feed those
  // byte-string helpers, so they carry no ASCII limit (matching the
  // block-scope `emitStringArrayInit` policy).
  if (init) {
    if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(
            init->IgnoreParenImpCasts())) {
      if (!literal->isWide())
        for (unsigned i = 0, n = literal->getLength(); i != n; ++i)
          if (literal->getCodeUnit(i) > 127)
            return emitError(initLoc)
                   << "unsupported: non-ASCII byte in string literal "
                      "initializer";
    }
  }
  // A function-pointer global initializer is either the null constant
  // (`None`) or a direct function reference (`Some(name)`, after the
  // signature check); both are emitted as opaque attributes.
  if (auto fnPtrType = llvm::dyn_cast<emitrust::FnPtrType>(type)) {
    const clang::Expr *e = stripTrivia(init);
    if (e->isNullPointerConstant(astContext(),
                                 clang::Expr::NPC_NeverValueDependent) !=
        clang::Expr::NPCK_NotNull)
      return Attribute(
          emitrust::OpaqueAttr::get(builder.getContext(), "None"));
    // A fn-ptr-to-fn-ptr conversion (prototype-less pointer bound to a
    // prototyped function) is transparent here; the signature check below
    // runs against the global's own fn_ptr type.
    if (const auto *bitcast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
      if (bitcast->getCastKind() == clang::CK_BitCast &&
          isFunctionPointer(bitcast->getSubExpr()->getType()))
        e = stripTrivia(bitcast->getSubExpr());
    const clang::Expr *fnExpr = nullptr;
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
      if (cast->getCastKind() == clang::CK_FunctionToPointerDecay)
        fnExpr = cast->getSubExpr();
    } else if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e)) {
      if (unary->getOpcode() == clang::UO_AddrOf)
        fnExpr = unary->getSubExpr();
    }
    if (!fnExpr)
      return emitError(initLoc)
             << "unsupported: global function pointer initializer";
    FailureOr<std::string> name =
        resolveFunctionPointerTarget(fnExpr, fnPtrType, initLoc);
    if (failed(name))
      return failure();
    return Attribute(emitrust::OpaqueAttr::get(
        builder.getContext(), (llvm::Twine("Some(") + *name + ")").str()));
  }
  // Static storage duration requires a constant initializer (C11 6.7.9p4);
  // clang's constant evaluator produces the folded value. For aggregates
  // it also resolves designators and zero-fills the uninitialized holes,
  // so the APValue is the complete element-by-element picture.
  clang::APValue *value = decl->evaluateValue();
  if (!value)
    return emitError(initLoc) << "unsupported: non-constant global initializer";
  return convertAPValueInit(*value, type, decl->getType(), initLoc);
}

FailureOr<Attribute> CImporter::convertAPValueInit(const clang::APValue &value,
                                                   Type type,
                                                   clang::QualType cType,
                                                   Location loc) {
  // A data-pointer struct member is stored as a plain i64 cursor field
  // whose degenerate binding carries no runtime information: the constant
  // initializer's lvalue (or null) converts to 0, and the binding itself
  // is recorded by `collectGlobalMemberBindings` at the object's import.
  // An admitted `void *` fn-ptr member (CTS-BR, 00216) folds like a
  // genuinely fn-ptr-typed field: its converted type is already the
  // fn_ptr, so it must not fall into the data-pointer i64 shortcut.
  if (!cType.isNull() && isDataPointer(cType) &&
      !llvm::isa<emitrust::FnPtrType>(type)) {
    auto intType = llvm::dyn_cast<IntegerType>(type);
    if (!intType || intType.getWidth() != 64 ||
        (!value.isLValue() && !value.isNullPointer()))
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    return Attribute(IntegerAttr::get(intType, 0));
  }
  if (auto intType = llvm::dyn_cast<IntegerType>(type)) {
    if (!value.isInt())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    if (intType.getWidth() == 1)
      return Attribute(builder.getBoolAttr(value.getInt().getBoolValue()));
    return Attribute(IntegerAttr::get(
        intType, value.getInt().extOrTrunc(intType.getWidth())));
  }
  if (auto floatType = llvm::dyn_cast<FloatType>(type)) {
    if (!value.isFloat())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    // A long double initializer arrives as an x87 APFloat; floatAttrFor
    // narrows it to the f64 the type policy substitutes (CTS 00204).
    return Attribute(floatAttrFor(floatType, value.getFloat()));
  }
  if (auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(type)) {
    if (!value.isArray() || value.getArraySize() != arrayType.getSize())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    const clang::ArrayType *cArray =
        cType.isNull() ? nullptr : astContext().getAsArrayType(cType);
    clang::QualType cElement =
        cArray ? cArray->getElementType() : clang::QualType();
    Type elementType = arrayType.getElementType();
    SmallVector<Attribute> elements;
    elements.reserve(arrayType.getSize());
    for (unsigned i = 0, n = value.getArrayInitializedElts(); i != n; ++i) {
      FailureOr<Attribute> element = convertAPValueInit(
          value.getArrayInitializedElt(i), elementType, cElement, loc);
      if (failed(element))
        return failure();
      elements.push_back(*element);
    }
    // Elements beyond the explicitly initialized prefix share the filler
    // value (C99 zero-fill of partial and designated initialization).
    if (elements.size() < arrayType.getSize()) {
      if (!value.hasArrayFiller())
        return emitError(loc)
               << "unsupported: global initializer does not match its type";
      FailureOr<Attribute> filler = convertAPValueInit(
          value.getArrayFiller(), elementType, cElement, loc);
      if (failed(filler))
        return failure();
      elements.append(arrayType.getSize() - elements.size(), *filler);
    }
    return Attribute(builder.getArrayAttr(elements));
  }
  if (auto structType = llvm::dyn_cast<emitrust::StructType>(type)) {
    auto structDef = llvm::dyn_cast_or_null<emitrust::StructDefOp>(
        SymbolTable::lookupSymbolIn(module, structType.getName()));
    if (!structDef)
      return emitError(loc)
             << "unsupported: global initializer for this type";
    // A union global carries a Union APValue, not a Struct one; it
    // initializes its single storage slot (the union imports as a
    // one-field struct, see `collectUnionSlot`) exactly like an
    // anonymous union member's slot, bit-exact across a signedness or
    // float pun.
    if (const clang::RecordDecl *unionRecord =
            structDefRecords.lookup(structType.getName());
        unionRecord && unionRecord->isUnion()) {
      ArrayAttr slotTypes = structDef.getFieldTypes();
      if (slotTypes.size() != 1)
        return emitError(loc)
               << "unsupported: global initializer does not match its type";
      Type slotType = llvm::cast<TypeAttr>(slotTypes[0]).getValue();
      FailureOr<Attribute> slot =
          convertAnonymousSlotInit(value, unionRecord, slotType, loc);
      if (failed(slot))
        return failure();
      return Attribute(builder.getArrayAttr({*slot}));
    }
    if (!value.isStruct())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    const clang::RecordDecl *record =
        cType.isNull() ? nullptr : recordOfType(cType);
    SmallVector<clang::QualType> cFields;
    if (record)
      for (const clang::FieldDecl *field : record->fields())
        cFields.push_back(field->getType());
    ArrayAttr fieldTypes = structDef.getFieldTypes();
    SmallVector<Attribute> fields;
    fields.reserve(fieldTypes.size());
    // An imported record converts field by field along the C structure,
    // which resolves flattened anonymous members; the struct_def's
    // flattened type list is consumed in step. Synthesized struct_defs
    // (owner structs, which never carry a C initializer in practice)
    // have no record and keep the positional conversion.
    if (const clang::RecordDecl *record =
            structDefRecords.lookup(structType.getName())) {
      unsigned typeIndex = 0;
      if (failed(convertRecordAPValue(value, record, fieldTypes, typeIndex,
                                      fields, loc)))
        return failure();
      if (typeIndex != fieldTypes.size() || fields.size() != fieldTypes.size())
        return emitError(loc)
               << "unsupported: global initializer does not match its type";
      return Attribute(builder.getArrayAttr(fields));
    }
    if (value.getStructNumFields() != fieldTypes.size())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    for (auto [i, fieldType] : llvm::enumerate(fieldTypes)) {
      FailureOr<Attribute> field = convertAPValueInit(
          value.getStructField(i),
          llvm::cast<TypeAttr>(fieldType).getValue(),
          i < cFields.size() ? cFields[i] : clang::QualType(), loc);
      if (failed(field))
        return failure();
      fields.push_back(*field);
    }
    return Attribute(builder.getArrayAttr(fields));
  }
  // A function-pointer element (a fn_ptr struct field, CTS-L3): the
  // evaluator yields an lvalue whose base is the target function
  // declaration (or the null constant, `None`). The signature check is the
  // same one every fn_ptr constant goes through.
  if (auto fnPtrType = llvm::dyn_cast<emitrust::FnPtrType>(type)) {
    if (!value.isLValue())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    if (value.isNullPointer())
      return Attribute(
          emitrust::OpaqueAttr::get(builder.getContext(), "None"));
    const auto *callee = llvm::dyn_cast_or_null<clang::FunctionDecl>(
        value.getLValueBase().dyn_cast<const clang::ValueDecl *>());
    if (!callee || !value.getLValueOffset().isZero())
      return emitError(loc)
             << "unsupported: global function pointer initializer";
    FailureOr<std::string> name =
        resolveFunctionPointerDecl(callee, fnPtrType, loc);
    if (failed(name))
      return failure();
    return Attribute(emitrust::OpaqueAttr::get(
        builder.getContext(), (llvm::Twine("Some(") + *name + ")").str()));
  }
  return emitError(loc) << "unsupported: global initializer for this type";
}

LogicalResult CImporter::convertRecordAPValue(
    const clang::APValue &value, const clang::RecordDecl *record,
    ArrayAttr fieldTypes, unsigned &typeIndex,
    SmallVectorImpl<Attribute> &fields, Location loc) {
  if (!value.isStruct())
    return emitError(loc)
           << "unsupported: global initializer does not match its type";
  unsigned valueIndex = 0;
  for (const clang::FieldDecl *field : record->fields()) {
    // A bit-field member has no field of its own in the flattened
    // struct_def; constant initialization of one is out of the C99-45
    // scope (packing the APValue bits is unimplemented).
    if (field->isBitField())
      return emitError(loc)
             << "unsupported: global initializer for a struct with "
                "bit-fields";
    if (valueIndex >= value.getStructNumFields())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    // A flexible array member or GNU zero-length array member has no
    // field in the struct_def (CTS-BR, 00216); its APValue slot is
    // consumed without emitting anything. A non-empty FAM-tail constant
    // on a TYPED record would silently vanish, so it stays rejected.
    if (field->getType()->isIncompleteArrayType() ||
        isZeroLengthArrayType(field->getType())) {
      const clang::APValue &dropped = value.getStructField(valueIndex++);
      if (field->getType()->isIncompleteArrayType() && dropped.isArray() &&
          dropped.getArraySize() > 0)
        return emitError(loc)
               << "unsupported: flexible array member initializer";
      continue;
    }
    const clang::APValue &fieldValue = value.getStructField(valueIndex++);
    if (field->isAnonymousStructOrUnion()) {
      const clang::RecordDecl *member =
          field->getType()->getAsRecordDecl()->getDefinition();
      if (member->isUnion()) {
        if (typeIndex >= fieldTypes.size())
          return emitError(loc)
                 << "unsupported: global initializer does not match its type";
        Type slotType =
            llvm::cast<TypeAttr>(fieldTypes[typeIndex++]).getValue();
        FailureOr<Attribute> slot =
            convertAnonymousSlotInit(fieldValue, member, slotType, loc);
        if (failed(slot))
          return failure();
        fields.push_back(*slot);
        continue;
      }
      if (failed(convertRecordAPValue(fieldValue, member, fieldTypes,
                                      typeIndex, fields, loc)))
        return failure();
      continue;
    }
    if (typeIndex >= fieldTypes.size())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    Type fieldType = llvm::cast<TypeAttr>(fieldTypes[typeIndex++]).getValue();
    FailureOr<Attribute> attr =
        convertAPValueInit(fieldValue, fieldType, field->getType(), loc);
    if (failed(attr))
      return failure();
    fields.push_back(*attr);
  }
  return success();
}

FailureOr<Attribute>
CImporter::convertAnonymousSlotInit(const clang::APValue &value,
                                    const clang::RecordDecl *record,
                                    Type slotType, Location loc) {
  if (record->isUnion()) {
    if (!value.isUnion())
      return emitError(loc)
             << "unsupported: global initializer does not match its type";
    const clang::FieldDecl *active = value.getUnionField();
    if (!active) {
      // No arm was initialized: the slot takes its zero value, matching
      // C's zero-fill of static storage.
      Attribute zero = builder.getZeroAttr(slotType);
      if (!zero)
        return emitError(loc)
               << "unsupported: global initializer does not match its type";
      return zero;
    }
    if (active->isAnonymousStructOrUnion())
      return convertAnonymousSlotInit(
          value.getUnionValue(),
          active->getType()->getAsRecordDecl()->getDefinition(), slotType,
          loc);
    // A float-pun arm's constant crosses the domain at compile time: the
    // active arm's value lands on the slot as its exact bit pattern (the
    // constant counterpart of the `emitrust.bitcast` at access sites).
    // Same-width int arms need no special case — the IntegerAttr path of
    // `convertAPValueInit` is already bit-exact (extOrTrunc).
    const clang::APValue &armValue = value.getUnionValue();
    if (auto slotInt = llvm::dyn_cast<IntegerType>(slotType);
        slotInt && armValue.isFloat()) {
      llvm::APInt bits = armValue.getFloat().bitcastToAPInt();
      if (bits.getBitWidth() != slotInt.getWidth())
        return emitError(loc)
               << "unsupported: global initializer does not match its type";
      return Attribute(IntegerAttr::get(slotInt, bits));
    }
    if (auto slotFloat = llvm::dyn_cast<FloatType>(slotType);
        slotFloat && armValue.isInt()) {
      if (armValue.getInt().getBitWidth() != slotFloat.getWidth())
        return emitError(loc)
               << "unsupported: global initializer does not match its type";
      return Attribute(FloatAttr::get(
          slotFloat, llvm::APFloat(slotFloat.getFloatSemantics(),
                                   armValue.getInt())));
    }
    return convertAPValueInit(armValue, slotType, active->getType(), loc);
  }
  // A nested anonymous struct on the slot path has exactly one field
  // (`anonymousUnionArmLeaf` admitted the arm); descend into it.
  if (!value.isStruct() || value.getStructNumFields() == 0)
    return emitError(loc)
           << "unsupported: global initializer does not match its type";
  const clang::FieldDecl *only = *record->field_begin();
  const clang::APValue &fieldValue = value.getStructField(0);
  if (only->isAnonymousStructOrUnion())
    return convertAnonymousSlotInit(
        fieldValue, only->getType()->getAsRecordDecl()->getDefinition(),
        slotType, loc);
  return convertAPValueInit(fieldValue, slotType, only->getType(), loc);
}

const GlobalInfo *CImporter::lookupGlobal(const clang::ValueDecl *decl) const {
  const auto *var = llvm::dyn_cast<clang::VarDecl>(decl);
  if (!var)
    return nullptr;
  auto it = globals.find(var->getCanonicalDecl());
  return it == globals.end() ? nullptr : &it->second;
}

const clang::VarDecl *
CImporter::asDirectGlobalRef(const clang::Expr *expr) const {
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr->IgnoreParens());
  if (!ref)
    return nullptr;
  const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
  if (!var)
    return nullptr;
  const clang::VarDecl *canonical = var->getCanonicalDecl();
  return globals.contains(canonical) ? canonical : nullptr;
}

bool CImporter::rootsAtGlobal(const clang::Expr *expr) const {
  const clang::Expr *e = expr->IgnoreParens();
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e))
    return lookupGlobal(ref->getDecl()) != nullptr;
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e))
    return !member->isArrow() && rootsAtGlobal(member->getBase());
  if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e))
    return rootsAtGlobal(subscript->getBase()->IgnoreParenImpCasts());
  return false;
}

LogicalResult
CImporter::flushGlobalWriteback(Location loc,
                                const GlobalWriteback &writeback) {
  if (!writeback.place)
    return success();
  if (!writeback.multiBases.empty()) {
    // A staged multi-base element (CTS-P7): dispatch on the staged
    // discriminant and store the mutated element back into the active
    // base at the staged cursor. A global-member base's arm (CTS-P9)
    // stages the global's whole value afresh, assigns the projected
    // member, and stores the whole value back.
    Value value = loadPlace(loc, writeback.place);
    return emitMultiBaseDispatch(
        loc, writeback.multiBases, writeback.multiBaseIndex,
        [&](const PointerBaseKey &base) -> LogicalResult {
          if (base.var->hasLocalStorage()) {
            FailureOr<Value> element = materializeLocalElementPlace(
                loc, base, writeback.multiCursor,
                writeback.multiPointeeType);
            if (failed(element))
              return failure();
            return storeToPlace(loc, *element, value);
          }
          FailureOr<std::pair<Value, std::string>> staged =
              stageGlobalCopy(loc, base.var);
          if (failed(staged))
            return failure();
          Value place = staged->first;
          if (base.member) {
            FailureOr<Value> memberPlace =
                projectMemberPlace(loc, place, base.member);
            if (failed(memberPlace))
              return failure();
            place = *memberPlace;
          }
          FailureOr<Value> element = refineElementPlace(
              loc, place, writeback.multiCursor, writeback.multiPointeeType);
          if (failed(element))
            return failure();
          if (failed(storeToPlace(loc, *element, value)))
            return failure();
          auto lvalueType =
              llvm::cast<emitrust::LValueType>(staged->first.getType());
          Value full = builder
                           .create<emitrust::LoadOp>(
                               loc, lvalueType.getValueType(), staged->first)
                           .getResult();
          builder.create<emitrust::GlobalStoreOp>(
              loc, full, globalSymbol(staged->second));
          return success();
        });
  }
  auto lvalueType =
      llvm::cast<emitrust::LValueType>(writeback.place.getType());
  Value full = builder
                   .create<emitrust::LoadOp>(loc, lvalueType.getValueType(),
                                             writeback.place)
                   .getResult();
  builder.create<emitrust::GlobalStoreOp>(loc, full,
                                          globalSymbol(writeback.symbol));
  return success();
}

LogicalResult CImporter::commitGlobalWriteback(
    Location loc, const GlobalWriteback &writeback, bool refreshStaged,
    llvm::function_ref<LogicalResult()> mutate) {
  // Refresh a stale single-base staged copy: the staging load ran when
  // the LHS place was formed, and any global write emitted since (an RHS
  // call mutating another subobject of the same global) would be
  // reverted by flushing that stale whole-value snapshot. Rebinding the
  // staged place to a fresh snapshot immediately before the mutation
  // keeps the user-visible evaluation order identical — every
  // subexpression value was already materialized — while making the
  // store-back exact. Statements without side-effecting subexpressions
  // skip the refresh: nothing can have written the global since staging.
  if (refreshStaged && writeback.place && writeback.multiBases.empty() &&
      !writeback.symbol.empty()) {
    auto lvalueType =
        llvm::cast<emitrust::LValueType>(writeback.place.getType());
    Value fresh = builder
                      .create<emitrust::GlobalLoadOp>(
                          loc, lvalueType.getValueType(),
                          globalSymbol(writeback.symbol))
                      .getResult();
    builder.create<emitrust::AssignOp>(loc, writeback.place, fresh);
  }
  if (failed(mutate()))
    return failure();
  return flushGlobalWriteback(loc, writeback);
}

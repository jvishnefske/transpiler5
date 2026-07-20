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
/// importTranslationUnit, and the low-level function-body-plumbing helpers
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
  llvm::StringRef cName = func->getName();
  if (cName == "main")
    return "c_main";
  // A function whose C spelling is a Rust keyword mangles like a struct
  // member — one trailing underscore (`match` -> `match_`, CTS 00204).
  // The mangled spelling is the symbol's identity everywhere (definition
  // and call sites resolve through this same function); a collision with
  // an existing `match_` is rejected in `importFunction`.
  std::string base = mangleMemberName(cName);
  // Internal-linkage (`static`) functions are mangled with the per-TU tag so
  // identically named file-statics in different TUs never collide. The tag is
  // empty for a single-TU import, preserving the historical bare name.
  if (func->getStorageClass() == clang::SC_Static)
    return currentTuTag + base;
  return base;
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

LogicalResult CImporter::importFunction(const clang::FunctionDecl *func) {
  Location loc = translateLoc(func->getLocation());
  llvm::StringRef cName = func->getName();

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

  bool isDefinition = func->isThisDeclarationADefinition();
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
  std::string name = mlirFuncName(func);

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
    if (argvParam->isReferenced() || argvParam->isUsed())
      return emitError(translateLoc(argvParam->getLocation()))
             << "unsupported: use of main's argv parameter (command-line "
                "argument values are not modeled)";
    inputTypes.push_back(builder.getIntegerType(32));
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
      // A planned string-cursor parameter (CTS 00204) lowers to TWO
      // inputs: a shared byte-slice over the region and an in-out i64
      // cursor. The advancement `*s = p` becomes a cursor write the
      // caller observes through the reference. Mutually exclusive with
      // the inferred-fn-ptr class above (different parameter types).
      if (cursorParams.contains(param)) {
        inputTypes.push_back(emitrust::RefType::get(
            emitrust::SliceType::get(builder.getIntegerType(8))));
        inputTypes.push_back(
            emitrust::MutRefType::get(builder.getIntegerType(64)));
        continue;
      }
      FailureOr<Type> paramType =
          mapParamType(param->getType(), translateLoc(param->getLocation()),
                       paramKinds[index]);
      if (failed(paramType))
        return failure();
      inputTypes.push_back(*paramType);
    }
  }
  SmallVector<Type> resultTypes;
  clang::QualType returnType = func->getReturnType();
  if (!returnType->isVoidType()) {
    // Returning an owned stream handle would let it escape its function
    // (C99-48 v1: no escapes); checked before the data-pointer return
    // classification below.
    if (isFilePtrType(returnType))
      return emitError(loc)
             << "unsupported: FILE* cannot cross a user-defined function "
                "boundary";
    if (isDataPointer(returnType)) {
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
  FunctionType functionType = builder.getFunctionType(inputTypes, resultTypes);

  // Reconcile with an earlier import of the same symbol. Across TUs an
  // external prototype in one file is satisfied by the definition in another;
  // a second definition of the same external symbol is a duplicate. (Internal
  // statics are mangled per-TU, so any collision here is a genuine external
  // clash — for a valid single TU clang has already merged redeclarations.)
  if (func::FuncOp existing = functions.lookup(name)) {
    if (!isDefinition)
      return success(); // Redundant declaration.
    if (!existing.isExternal())
      return emitError(loc)
             << "unsupported: conflicting definition of '" << name
             << "' (already defined in another translation unit)";
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
    existing.erase();
    functions.erase(name);
  }

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(module.getBody());
  auto funcOp = builder.create<func::FuncOp>(loc, name, functionType);
  if (methodOwner)
    funcOp->setAttr(emitrust::kMethodOfAttrName,
                    builder.getStringAttr(ownerStructType.getName()));
  functions[name] = funcOp;
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
  pointerPointerLocals.clear();
  carrierLocals.clear();
  carrierParams.clear();
  literalBackings.clear();
  paramCells.clear();
  ownerStructPlaces.clear();
  loopStack.clear();
  labelBlocks.clear();
  switchCaseBlocks.clear();
  inferredFnPtrSigs = std::move(inferredSigs);
  cursorWritebacks.clear();
  currentVaCloneActive = false;
  currentVaExtras.clear();
  currentVaCursorCell = Value();
  currentHasLabels = containsLabelStmt(func->getBody());
  currentFunctionBody = func->getBody();
  currentReceiverPlace = Value();
  currentMethodOwner = nullptr;
  currentReturnType = resultTypes.empty() ? Type() : resultTypes.front();
  // An erased single-global-base pointer return (CTS-S, 00089): return
  // sites emit a bare `return` instead of the classified `&global`.
  currentErasedReturnBase =
      globalReturnBases.lookup(func->getCanonicalDecl());
  currentFuncName = name;
  currentIsMain = name == "c_main";
  bodyRegion = &funcOp.getBody();
  entryBlock = funcOp.addEntryBlock();
  builder.setInsertionPointToStart(entryBlock);
  collectAddressTaken(func->getBody());
  // Calls to carrier-returning functions are carrier sources of the
  // assigned pointer's region (CTS-P3).
  pointerRegions.carrierReturnQuery =
      [this](const clang::FunctionDecl *callee) {
        return isCarrierReturnFunction(callee);
      };
  // Admitted local `void *` fn-ptr holders (CTS-F, 00210) import as
  // ordinary fn_ptr locals; the pointer decomposition never tracks them.
  collectVoidFnPtrHolders(func->getBody());
  pointerRegions.fnHolderQuery = [this](const clang::VarDecl *var) {
    return voidFnPtrHolders.contains(var);
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

  unsigned entryArgIndex = methodOwner ? 1 : 0;
  for (const clang::ParmVarDecl *param : func->parameters()) {
    // main's `argv` was dropped from the imported signature (it has no
    // entry-block argument); its uses were rejected at signature time, so
    // no binding is needed.
    if (currentIsMain && isPointerType(param->getType()))
      continue;
    Location paramLoc = translateLoc(param->getLocation());
    // A string-cursor parameter (CTS 00204) owns TWO entry-block
    // arguments: the shared region slice and the in-out cursor.
    if (cursorParams.contains(param)) {
      Value baseArg = entryBlock->getArgument(entryArgIndex);
      Value cursorArg = entryBlock->getArgument(entryArgIndex + 1);
      entryArgIndex += 2;
      if (failed(bindCursorParam(param, baseArg, cursorArg, paramLoc)))
        return failure();
      continue;
    }
    Value blockArg = entryBlock->getArgument(entryArgIndex++);
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

  if (failed(emitStmt(func->getBody())))
    return failure();
  return finalizeFunction(funcOp, loc);
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
                emitrust::FnPtrType>(type) ||
      isUnsignedInt(type) || addressTaken.contains(param)) {
    // By-value struct, enum, function pointer, or unsigned scalar, or an
    // address-taken scalar: copy into a Rust variable (dialect-typed
    // values must not become memref cells — a memref of a dialect type
    // is illegal — and unsigned cells must not either, because mem2reg
    // materializes its default value as an `arith.constant`, which
    // requires a signless type).
    Value place = builder
                      .create<emitrust::VariableOp>(
                          paramLoc, emitrust::LValueType::get(type))
                      .getResult();
    builder.create<emitrust::AssignOp>(paramLoc, place, blockArg);
    symbols[param] = place;
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

LogicalResult CImporter::bindCursorParam(const clang::ParmVarDecl *param,
                                         Value baseArg, Value cursorArg,
                                         Location paramLoc) {
  // The shared byte-slice argument derefs once into the region base
  // place, exactly like a slice parameter's; reads render
  // `(*base)[i as usize]` and never hold a borrow across statements.
  auto sliceType = emitrust::SliceType::get(builder.getIntegerType(8));
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
      inputTypes.push_back(emitrust::RefType::get(
          emitrust::SliceType::get(builder.getIntegerType(8))));
      inputTypes.push_back(
          emitrust::MutRefType::get(builder.getIntegerType(64)));
      continue;
    }
    FailureOr<Type> paramType =
        mapParamType(param->getType(), translateLoc(param->getLocation()),
                     paramKinds[index]);
    if (failed(paramType))
      return failure();
    inputTypes.push_back(*paramType);
  }
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
  currentVaCloneActive = true;
  currentVaExtras.clear();
  currentHasLabels = containsLabelStmt(func->getBody());
  currentFunctionBody = func->getBody();
  currentReceiverPlace = Value();
  currentMethodOwner = nullptr;
  currentReturnType = resultTypes.empty() ? Type() : resultTypes.front();
  currentErasedReturnBase = nullptr;
  currentFuncName = clone.name;
  currentIsMain = false;
  bodyRegion = &funcOp.getBody();
  entryBlock = funcOp.addEntryBlock();
  builder.setInsertionPointToStart(entryBlock);
  collectAddressTaken(func->getBody());
  pointerRegions.carrierReturnQuery =
      [this](const clang::FunctionDecl *callee) {
        return isCarrierReturnFunction(callee);
      };
  collectVoidFnPtrHolders(func->getBody());
  pointerRegions.fnHolderQuery = [this](const clang::VarDecl *var) {
    return voidFnPtrHolders.contains(var);
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

LogicalResult CImporter::importTranslationUnit(clang::ASTContext &context,
                                               llvm::StringRef tuTag,
                                               bool deferExtern,
                                               bool soleTranslationUnit) {
  astContextPtr = &context;
  currentTuTag = tuTag.str();
  deferExternGlobals = deferExtern;
  currentSoleTU = soleTranslationUnit;
  const clang::TranslationUnitDecl *unit = astContext().getTranslationUnitDecl();
  // Namespace pre-pass: record every module-symbol name this TU's ordinary
  // identifier namespace will claim, so struct tag naming
  // (`structSymbolName`) is independent of declaration order.
  collectOrdinaryNames(unit);
  // Phase-4 Pass A: pure-AST owner planning over every function definition
  // before any IR is built; Pass B below consults the plans.
  planOwners(unit, soleTranslationUnit);
  // CTS-P10 Pass A: cell-slice classification of pointer-parameter
  // classes whose bases are all mutable global arrays.
  planCellSlices(unit, soleTranslationUnit);
  // CTS-S Pass A: per-TU fn-ptr facts (written globals, address-taken
  // functions) and the devirtualization aliases of never-reassigned
  // global function pointers.
  planFnPtrAliases(unit);
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
  for (const clang::Decl *decl : unit->decls()) {
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
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (failed(importFunction(func)))
        return failure();
      continue;
    }
    if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(decl)) {
      // An EMPTY struct that no declaration type mentions is skipped: it
      // may only ever appear as a zero-byte member of a byte-region
      // aggregate (CTS-BR, 00216), which never materializes the record
      // type at all. One that IS declared with keeps the eager import.
      if (const clang::RecordDecl *definition = record->getDefinition();
          definition && definition->isStruct() && definition->field_empty() &&
          !declTypeUsedRecords.contains(definition))
        continue;
      if (failed(importRecord(record, translateLoc(record->getBeginLoc()))))
        return failure();
      continue;
    }
    if (const auto *enumDecl = llvm::dyn_cast<clang::EnumDecl>(decl)) {
      if (failed(importEnum(enumDecl, translateLoc(enumDecl->getBeginLoc()))))
        return failure();
      continue;
    }
    if (llvm::isa<clang::TypedefDecl>(decl) || llvm::isa<clang::EmptyDecl>(decl))
      continue;
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
      if (failed(importGlobalVar(var)))
        return failure();
      continue;
    }
    return emitError(translateLoc(decl->getBeginLoc()))
           << "unsupported top-level declaration";
  }
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
    // ev == P; an exact power of ten keeps it). Non-finite values pad
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
            "    let ev: i32 = match e.parse() {\n"
            "        Ok(v) => v,\n"
            "        Err(_) => 0,\n"
            "    };\n"
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
            "                let mut m = if carried && ev == pp as i32 {\n"
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

Value CImporter::createVariablePlace(Location loc, Type type) {
  OpBuilder::InsertionGuard guard(builder);
  if (currentHasLabels)
    builder.setInsertionPointToStart(entryBlock);
  return builder
      .create<emitrust::VariableOp>(loc, emitrust::LValueType::get(type))
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

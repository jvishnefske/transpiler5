//===- ImportCTypes.cpp - C type mapping for the importer -----*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// CImporter's C-to-MLIR type mapping: mapType/mapParamType/
/// mapStructFieldType, the classifyPointerReturn/classifyPointerParams/
/// classifyFnPtrPointerResult pointer-shape classification family, and the
/// small location/system-header utilities. Split out of ImportC.cpp by pure
/// code motion (W1.6); see CImporterInternal.h for the CImporter class
/// declaration this file implements.
//
//===----------------------------------------------------------------------===//

#include "CImporterInternal.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// Locations and types
//===----------------------------------------------------------------------===//

Location CImporter::translateLoc(clang::SourceLocation sourceLoc) {
  MLIRContext *context = builder.getContext();
  if (sourceLoc.isInvalid())
    return UnknownLoc::get(context);
  const clang::SourceManager &sourceManager = astContext().getSourceManager();
  clang::PresumedLoc presumed = sourceManager.getPresumedLoc(sourceLoc);
  if (presumed.isInvalid())
    return UnknownLoc::get(context);
  return FileLineColLoc::get(StringAttr::get(context, presumed.getFilename()),
                             presumed.getLine(), presumed.getColumn());
}

bool CImporter::isSystemHeaderDecl(const clang::Decl *decl) const {
  const clang::SourceManager &sourceManager = astContext().getSourceManager();
  clang::SourceLocation loc =
      sourceManager.getExpansionLoc(decl->getLocation());
  return loc.isValid() && sourceManager.isInSystemHeader(loc);
}

LogicalResult CImporter::rejectSystemHeaderUse(Location loc,
                                               llvm::StringRef what,
                                               llvm::StringRef name) {
  return emitError(loc) << "unsupported: " << what << " '" << name
                        << "' declared in a system header; not part of the "
                           "supported C subset";
}

FailureOr<Type> CImporter::mapType(clang::QualType type, Location loc) {
  clang::QualType canonical = type.getCanonicalType();

  // C99-7 qualifier policy. volatile: located rejection (no Rust
  // counterpart in the emitted model). _Atomic: located rejection (no
  // atomics in the single-threaded model; checked here for a precise
  // message instead of the generic tail rejection). const: no effect on
  // the value type — the const mapping is positional (immutable statics
  // for never-written globals, const-marked variables for literal
  // backings, plain SSA lets after mem2reg). restrict: accepted and
  // ignored (an aliasing hint; the pointer region analysis is stricter).
  if (canonical.isVolatileQualified())
    return emitError(loc) << "unsupported: volatile-qualified type";
  if (canonical->isAtomicType())
    return emitError(loc) << "unsupported: _Atomic-qualified type";
  // W2.0: a C++ reference (lvalue `T&` or rvalue `T&&`) has no
  // representation in this model — checked with a sharpened, dedicated
  // message ahead of the generic tail rejection so a reference parameter
  // (the shape `mapParamType` falls through to this function for, since a
  // reference is not a pointer type) gets a clear diagnostic instead of
  // the generic "unsupported type '...'" spelling. References themselves
  // are out of scope for every wave through W2.0; only the located
  // rejection is pinned.
  if (canonical->isReferenceType())
    return emitError(loc) << "unsupported: reference types are not yet supported";

  // C99-37: va_list is a PERMANENT rejection — Rust has no stable
  // variadic-argument access, so the target's `__builtin_va_list` (and
  // its underlying `__va_list_tag` record) has no meaningful
  // representation in any position: local, parameter, field, or global.
  // A va_list touched inside a variadic DEFINITION is caught earlier by
  // the variadic-definition rejection (`bodyUsesVaList`); this check
  // catches the type escaping into non-variadic contexts, which would
  // otherwise import as a garbage register-save-area struct.
  {
    clang::ASTContext &context = astContext();
    if (context.hasSameType(canonical,
                            context.getBuiltinVaListType().getCanonicalType()))
      return emitError(loc) << "unsupported: va_list type";
    if (const auto *record = canonical->getAs<clang::RecordType>())
      if (const clang::Decl *tag = context.getVaListTagDecl())
        if (record->getDecl()->getCanonicalDecl() == tag->getCanonicalDecl())
          return emitError(loc) << "unsupported: va_list type";
  }

  if (const auto *builtin =
          llvm::dyn_cast<clang::BuiltinType>(canonical.getTypePtr())) {
    switch (builtin->getKind()) {
    case clang::BuiltinType::Bool:
      return Type(builder.getI1Type());
    case clang::BuiltinType::Char_S:
    case clang::BuiltinType::SChar:
      return Type(builder.getIntegerType(8));
    case clang::BuiltinType::Short:
      return Type(builder.getIntegerType(16));
    case clang::BuiltinType::Int:
      return Type(builder.getIntegerType(32));
    case clang::BuiltinType::Long:
    case clang::BuiltinType::LongLong:
      return Type(builder.getIntegerType(64));
    case clang::BuiltinType::Float:
      return Type(builder.getF32Type());
    case clang::BuiltinType::Double:
      return Type(builder.getF64Type());
    // CTS 00204 / C99-8 policy: `long double` maps to f64, the same type
    // as `double` (a UB refinement: C requires long double to be at least
    // as wide as double, every f64-exact value round-trips, and the
    // supported shapes perform no long-double-only arithmetic). The
    // conversions double <-> long double become identities, L-suffixed
    // literals convert their x87 APFloat to IEEE double at import, and
    // constructs that would observe the substitution stay rejected:
    // sizeof/alignof of long double (emitSizeofAlignof) and the printf
    // %La/%LA hex-float forms (translatePrintfFormat).
    case clang::BuiltinType::LongDouble:
      return Type(builder.getF64Type());
    // Unsigned types map to MLIR unsigned (not signless) integers so the
    // Rust emitter renders them as `uN`; arith ops require signless
    // operands, so all arithmetic on these goes through emitrust ops.
    case clang::BuiltinType::Char_U:
    case clang::BuiltinType::UChar:
      return Type(IntegerType::get(builder.getContext(), 8,
                                   IntegerType::Unsigned));
    case clang::BuiltinType::UShort:
      return Type(IntegerType::get(builder.getContext(), 16,
                                   IntegerType::Unsigned));
    case clang::BuiltinType::UInt:
      return Type(IntegerType::get(builder.getContext(), 32,
                                   IntegerType::Unsigned));
    case clang::BuiltinType::ULong:
    case clang::BuiltinType::ULongLong:
      return Type(IntegerType::get(builder.getContext(), 64,
                                   IntegerType::Unsigned));
    default:
      break;
    }
    return emitError(loc) << "unsupported builtin type '"
                          << llvm::Twine(canonical.getAsString()) << "'";
  }

  if (const auto *record =
          llvm::dyn_cast<clang::RecordType>(canonical.getTypePtr())) {
    const clang::RecordDecl *decl = record->getDecl();
    // A union imports as a one-field struct (see `collectUnionSlot`), so
    // it maps to the same `!emitrust.struct` any record does; shapes the
    // one-slot model cannot represent are rejected by the import below.
    const clang::RecordDecl *definition = decl->getDefinition();
    if (!definition)
      return emitError(loc) << "unsupported: incomplete struct type";
    // CTS-BR (00216): a u8-only aggregate is padding-free by construction
    // and imports as a BYTE REGION — a plain `!emitrust.array<Nxui8>`
    // where N == sizeof (a flexible array member contributes zero) — and
    // NO struct_def is ever emitted for the record.
    if (isByteRegionRecord(definition)) {
      uint64_t bytes =
          astContext().getTypeSizeInChars(canonical).getQuantity();
      return Type(emitrust::ArrayType::get(
          builder.getContext(), bytes,
          IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned)));
    }
    if (failed(importRecord(definition, loc)))
      return failure();
    // The type name is resolved after the import: a block-scope record
    // maps to its mangled per-declaration name (see localRecordNames), a
    // file-scope record to the collision-resolved name assigned by
    // `structSymbolName` during the import, and a bare anonymous struct
    // to its synthesized shape-keyed name.
    std::string structName = emittedRecordName(definition);
    if (structName.empty())
      return emitError(loc) << "unsupported: anonymous struct type";
    return Type(emitrust::StructType::get(builder.getContext(), structName));
  }

  if (const clang::ConstantArrayType *array =
          astContext().getAsConstantArrayType(canonical)) {
    // CTS-BR (00216): an array of byte-region records flattens to ONE
    // region of n*sizeof bytes — elements are consecutive padding-free
    // byte runs, so the flat region preserves every offset.
    if (isByteRegionAggregate(canonical)) {
      uint64_t bytes =
          astContext().getTypeSizeInChars(canonical).getQuantity();
      if (bytes > 0)
        return Type(emitrust::ArrayType::get(
            builder.getContext(), bytes,
            IntegerType::get(builder.getContext(), 8,
                             IntegerType::Unsigned)));
    }
    FailureOr<Type> element = mapType(array->getElementType(), loc);
    if (failed(element))
      return failure();
    uint64_t size = array->getSize().getZExtValue();
    if (size == 0)
      return emitError(loc) << "unsupported: zero-length array";
    // A multi-dimensional array recurses naturally: the element of the
    // outer dimension is itself an `!emitrust.array` (rendered as the
    // nested Rust array `[[T; N]; M]`). An array of function pointers is
    // a fn-ptr TABLE (CTS-BR, 00216): its never-reassigned global form
    // folds to a Some(target) element list, and runtime stores into a
    // slot are rejected at the assignment.
    return Type(emitrust::ArrayType::get(builder.getContext(), size, *element));
  }

  if (const auto *enumType =
          llvm::dyn_cast<clang::EnumType>(canonical.getTypePtr())) {
    const clang::EnumDecl *definition = enumType->getDecl()->getDefinition();
    if (!definition)
      return emitError(loc) << "unsupported: incomplete enum type";
    // Anonymous enums are plain `int` everywhere else in the importer
    // (enumerator references become `i32` constants at their use sites), so
    // a value of anonymous enum type is a plain `i32`.
    if (definition->getName().empty())
      return Type(builder.getIntegerType(32));
    if (failed(importEnum(definition, loc)))
      return failure();
    return Type(
        emitrust::EnumType::get(builder.getContext(), definition->getName()));
  }

  // Function pointers are ordinary `!emitrust.fn_ptr` values (rendered
  // `Option<fn(...)>`), legal as locals, globals, struct fields,
  // parameters, and results alike; they must be recognized before the
  // general pointer rejection below.
  if (canonical->isFunctionPointerType()) {
    const auto *fnType =
        canonical->getPointeeType()->castAs<clang::FunctionType>();
    SmallVector<Type> inputs;
    if (const auto *proto = llvm::dyn_cast<clang::FunctionProtoType>(fnType)) {
      if (proto->isVariadic())
        return emitError(loc) << "unsupported: variadic function pointer type";
      for (clang::QualType param : proto->getParamTypes()) {
        FailureOr<Type> mapped = mapType(param, loc);
        if (failed(mapped))
          return failure();
        if (!emitrust::FnPtrType::isValidComponentType(*mapped))
          return emitError(loc)
                 << "unsupported: function pointer parameter type";
        inputs.push_back(*mapped);
      }
    }
    // A prototype-less K&R `int (*f)()` maps to the zero-parameter form.
    // A local-storage decl whose call sites carry arguments is refined to
    // its callsite-inferred signature at declaration/parameter mapping
    // (FR-29, CTS 00209) and never consults this default; any remaining
    // argument-carrying call through an UNREFINED no-proto value is
    // rejected at the call site.
    SmallVector<Type> results;
    clang::QualType returnType = fnType->getReturnType();
    if (!returnType->isVoidType()) {
      if (isDataPointer(returnType)) {
        // A data-pointer fn-ptr result (CTS-S, 00089) is representable
        // only when every possible target erases it to one global base;
        // the result then erases from the fn_ptr signature too, and
        // indirect calls route to the base (`erasedGlobalReturnCallBase`).
        if (failed(classifyFnPtrPointerResult(fnType, loc)))
          return failure();
      } else {
        FailureOr<Type> mapped = mapType(returnType, loc);
        if (failed(mapped))
          return failure();
        if (!emitrust::FnPtrType::isValidComponentType(*mapped))
          return emitError(loc)
                 << "unsupported: function pointer result type";
        results.push_back(*mapped);
      }
    }
    return Type(
        emitrust::FnPtrType::get(builder.getContext(), inputs, results));
  }

  // A FILE* is an owned function-local stream handle (C99-48); any other
  // position (array element, return type, ...) that maps its type keeps
  // this located rejection.
  if (isFilePtrType(canonical))
    return emitError(loc)
           << "unsupported: FILE* is only supported as a function-local "
              "variable";
  if (canonical->isPointerType())
    return emitError(loc)
           << "unsupported: pointer type outside a parameter position";
  if (canonical->isArrayType())
    return emitError(loc) << "unsupported: non-constant array size";
  return emitError(loc) << "unsupported type '"
                        << llvm::Twine(canonical.getAsString()) << "'";
}

FailureOr<Type> CImporter::mapParamType(clang::QualType type, Location loc,
                                        ParamKind kind) {
  // C99-7: qualifiers on the parameter OBJECT itself are body-local and
  // never part of the function type (C11 6.7.6.3p15 composite rules;
  // `int x[volatile 5]` adjusts to `int * volatile x`, c-testsuite
  // 00162), so a top-level volatile is accepted and ignored exactly like
  // const and restrict. volatile anywhere deeper — the pointee chain,
  // which names caller-owned storage — keeps the located rejection; the
  // deep scan also covers the Carrier shape that bypasses `mapType`.
  clang::QualType canonical = type.getCanonicalType().getUnqualifiedType();
  if (canonical->isPointerType() && !canonical->isFunctionPointerType() &&
      hasVolatileQualifier(astContext(), canonical->getPointeeType()))
    return emitError(loc) << "unsupported: volatile-qualified type";
  // A function-pointer parameter is an ordinary Copy value, not a
  // reference; it maps to `!emitrust.fn_ptr` like every other position
  // (through the stripped canonical, so a qualifier on the parameter
  // object itself stays ignored).
  if (canonical->isFunctionPointerType())
    return mapType(canonical, loc);
  if (canonical->isPointerType()) {
    clang::QualType pointee = canonical->getPointeeType();
    // An integer-carrier `void *` parameter (CTS-P3) is a plain i64: the
    // callee only ever truth-tests it, so the value never needs a region.
    if (kind == ParamKind::Carrier)
      return Type(builder.getIntegerType(64));
    // A `void *` parameter has no element type to classify against and no
    // region to join at the call boundary (CTS-P9).
    if (pointee.getCanonicalType()->isVoidType())
      return emitError(loc) << "unsupported: void pointer parameter";
    // A pointee that is itself a *data* pointer has no representation
    // (CTS-P5); a function-pointer pointee is an ordinary Copy value
    // (`!emitrust.fn_ptr`) and slices/references over it are fine — the
    // shape a decayed array-of-function-pointers parameter produces.
    if (pointee.getCanonicalType()->isPointerType() &&
        !pointee.getCanonicalType()->isFunctionPointerType())
      return emitError(loc) << "unsupported: pointer-to-pointer parameter";
    // CTS-BR (00216): a pointer to a byte-region aggregate is a byte
    // slice parameter regardless of the body-usage classification —
    // member reads through it are region reads at constant byte offsets
    // over the slice base. A const pointee borrows shared.
    if (kind != ParamKind::CellSlice && isByteRegionAggregate(pointee)) {
      auto slice = emitrust::SliceType::get(IntegerType::get(
          builder.getContext(), 8, IntegerType::Unsigned));
      if (pointee.isConstQualified())
        return Type(emitrust::RefType::get(slice));
      return Type(emitrust::MutRefType::get(slice));
    }
    FailureOr<Type> inner = mapType(pointee, loc);
    if (failed(inner))
      return failure();
    if (kind == ParamKind::CellSlice) {
      // A cell-slice class parameter (CTS-P10): a shared reference to a
      // run of Cells over one of the class's mutable global array bases.
      if (!emitrust::CellSliceType::isValidElementType(*inner))
        return emitError(loc)
               << "unsupported: cell-slice parameter element type " << *inner;
      return Type(
          emitrust::RefType::get(emitrust::CellSliceType::get(*inner)));
    }
    if (kind == ParamKind::Slice) {
      // A slice element must be sized and scalar/struct; a pointer to an
      // array (`int (*)[N]`) has no slice shape.
      if (!emitrust::SliceType::isValidElementType(*inner))
        return emitError(loc)
               << "unsupported: slice parameter element type " << *inner;
      // CTS-BR (00216): a walked `const unsigned char *` is a SHARED
      // byte slice (`&[u8]`), the borrow shape the byte-region walkers
      // pass region views through.
      if (pointee.isConstQualified() && isU8ScalarType(pointee))
        return Type(
            emitrust::RefType::get(emitrust::SliceType::get(*inner)));
      return Type(
          emitrust::MutRefType::get(emitrust::SliceType::get(*inner)));
    }
    return Type(emitrust::MutRefType::get(*inner));
  }
  // The stripped canonical keeps a top-level-volatile value parameter
  // (`volatile int x`, a body-local copy) out of `mapType`'s rejection.
  return mapType(canonical, loc);
}

FailureOr<Type> CImporter::mapStructFieldType(clang::QualType type,
                                              Location loc) {
  // C99-7: a data-pointer field stores as a plain i64 without mapping its
  // pointee, so the volatile scan must run before that shortcut.
  if (hasVolatileQualifier(astContext(), type))
    return emitError(loc) << "unsupported: volatile-qualified type";
  if (isDataPointer(type))
    return Type(builder.getIntegerType(64));
  return mapType(type, loc);
}

ArrayRef<ParamKind>
CImporter::classifyPointerParams(const clang::FunctionDecl *func) {
  const clang::FunctionDecl *canonical = func->getCanonicalDecl();
  auto it = paramKindsCache.find(canonical);
  if (it != paramKindsCache.end())
    return it->second;
  SmallVector<ParamKind, 4> kinds(func->getNumParams(), ParamKind::ScalarRef);
  // The classification is a property of the definition's body; without a
  // definition in the merged ASTs every pointer parameter stays a scalar
  // reference (checked at definition-time signature refinement).
  const clang::FunctionDecl *definition = func->getDefinition();
  if (definition && definition->hasBody() &&
      definition->getNumParams() == kinds.size()) {
    llvm::SmallPtrSet<const clang::ParmVarDecl *, 4> sliceParams;
    collectSliceParams(definition->getBody(), sliceParams);
    for (auto [index, param] : llvm::enumerate(definition->parameters())) {
      if (sliceParams.contains(param))
        kinds[index] = ParamKind::Slice;
      // The interprocedural cell-slice class (CTS-P10, planned in Pass A)
      // overrides the per-body slice classification.
      if (cellSliceParams.contains(param))
        kinds[index] = ParamKind::CellSlice;
      // A `void *` parameter that the body only ever truth-tests is an
      // integer carrier (CTS-P3): it lowers as a plain i64 and call sites
      // pass carrier values. Any other `void *` parameter keeps the
      // historical rejection in `mapParamType`.
      if (isDataPointer(param->getType()) &&
          param->getType()
              .getCanonicalType()
              ->getPointeeType()
              .getCanonicalType()
              ->isVoidType() &&
          voidParamOnlyTruthTested(definition->getBody(), param))
        kinds[index] = ParamKind::Carrier;
    }
  }
  auto [entry, inserted] =
      paramKindsCache.try_emplace(canonical, std::move(kinds));
  (void)inserted;
  return entry->second;
}

FailureOr<Type> CImporter::classifyPointerReturn(
    const clang::FunctionDecl *func, Location loc) {
  const clang::FunctionDecl *canonical = func->getCanonicalDecl();
  auto it = pointerReturnKinds.find(canonical);
  if (it != pointerReturnKinds.end())
    return it->second;
  const clang::FunctionDecl *definition = func->getDefinition();
  if (!definition || !definition->hasBody())
    return emitError(loc) << "unsupported: pointer return type";
  SmallVector<const clang::ReturnStmt *> returns;
  collectReturnStmts(definition->getBody(), returns);
  if (returns.empty())
    return emitError(loc) << "unsupported: pointer return type";
  // The integer-carrier kind (CTS-P3): every return site yields an
  // integer riding in pointer clothing (a null constant, an
  // integer-to-pointer cast, a carrier-region local, or a call to another
  // carrier-returning function), so the function returns a plain i64.
  if (llvm::any_of(returns,
                   [](const clang::ReturnStmt *ret) {
                     return !ret->getRetValue() ||
                            !returnedFunctionExpr(ret->getRetValue());
                   }) &&
      isCarrierReturnFunction(func)) {
    Type kind = builder.getIntegerType(64);
    pointerReturnKinds.try_emplace(canonical, kind);
    return kind;
  }
  // The single-global-base RETURN region kind (CTS-S, 00089): one return
  // site yielding the address of a global claims the kind for the whole
  // function — every site must then return the SAME whole mutable global
  // (cursor 0) and no site may return NULL. The pointer result ERASES from
  // the signature (the classification is the null `Type`): no runtime
  // pointer state travels, the call is retained for its side effects, and
  // callers route accesses through the returned pointer to the global
  // directly (see `erasedGlobalReturnCallBase`).
  if (llvm::any_of(returns, [](const clang::ReturnStmt *ret) {
        return ret->getRetValue() &&
               returnedGlobalAddress(ret->getRetValue()).base;
      })) {
    const clang::VarDecl *commonBase = nullptr;
    Location memberSiteLoc = loc;
    bool sawMemberSite = false;
    for (const clang::ReturnStmt *ret : returns) {
      Location retLoc = translateLoc(ret->getReturnLoc());
      const clang::Expr *value = ret->getRetValue();
      if (!value)
        return emitError(retLoc) << "unsupported: returned pointer value "
                                    "(a bare return cannot carry the "
                                    "global address)";
      if (isNullPointerConstantExpr(value))
        return emitError(retLoc)
               << "unsupported: return sites mix a global address and NULL";
      ReturnedGlobalAddress site = returnedGlobalAddress(value);
      if (!site.base)
        return emitError(retLoc)
               << "unsupported: returned pointer value (only a returned "
                  "whole-global or function address has a representation)";
      if (commonBase && site.base != commonBase)
        return emitError(retLoc) << "unsupported: return sites disagree on "
                                    "the returned global base";
      commonBase = site.base;
      if (!site.wholeObject) {
        sawMemberSite = true;
        memberSiteLoc = retLoc;
      }
    }
    if (sawMemberSite)
      return emitError(memberSiteLoc)
             << "unsupported: returned pointer value (a member address is "
                "not a whole-global base)";
    if (commonBase->getType().isConstQualified())
      return emitError(loc)
             << "unsupported: returned address of a const global";
    // The erased routing stages and stores the base back at its own type,
    // so the returned pointee must be exactly the whole global's type.
    clang::QualType pointee = definition->getReturnType()
                                  .getCanonicalType()
                                  ->getPointeeType();
    if (!astContext().hasSameUnqualifiedType(pointee, commonBase->getType()))
      return emitError(loc) << "unsupported: returned pointer value (the "
                               "returned global does not match the "
                               "pointee type)";
    globalReturnBases[canonical] = commonBase;
    pointerReturnKinds.try_emplace(canonical, Type());
    return Type();
  }
  Type kind;
  for (const clang::ReturnStmt *ret : returns) {
    Location retLoc = translateLoc(ret->getReturnLoc());
    const clang::Expr *value = ret->getRetValue();
    const clang::Expr *fnExpr = value ? returnedFunctionExpr(value) : nullptr;
    if (!fnExpr)
      return emitError(retLoc)
             << "unsupported: returned pointer value (only a returned "
                "function address has a representation; a cursor into a "
                "callee-local region would dangle)";
    const auto *fn = llvm::cast<clang::FunctionDecl>(
        llvm::cast<clang::DeclRefExpr>(fnExpr)->getDecl());
    FailureOr<Type> mapped =
        mapType(astContext().getPointerType(fn->getType()), retLoc);
    if (failed(mapped))
      return failure();
    if (kind && kind != *mapped)
      return emitError(retLoc)
             << "unsupported: return sites disagree on the returned "
                "function pointer signature";
    kind = *mapped;
  }
  if (!kind)
    return emitError(loc) << "unsupported: pointer return type";
  pointerReturnKinds.try_emplace(canonical, kind);
  return kind;
}

FailureOr<const clang::VarDecl *>
CImporter::classifyFnPtrPointerResult(const clang::FunctionType *fnType,
                                      Location loc) {
  const clang::Type *key = astContext()
                               .getCanonicalType(clang::QualType(fnType, 0))
                               .getTypePtr();
  if (const clang::VarDecl *cached = fnPtrReturnBases.lookup(key))
    return cached;
  // The candidate set must be whole-program: in a multi-TU project another
  // TU could take a diverging function's address after this TU classified.
  if (!currentSoleTU)
    return emitError(loc) << "unsupported: function pointer result type";
  if (!fnPtrReturnInProgress.insert(key).second)
    return emitError(loc) << "unsupported: function pointer result type";
  auto eraseInProgress = [&]() { fnPtrReturnInProgress.erase(key); };
  clang::QualType returnType = fnType->getReturnType().getCanonicalType();
  SmallVector<const clang::FunctionDecl *, 4> candidates;
  for (const clang::FunctionDecl *fn : addressTakenFunctions)
    if (astContext().hasSameUnqualifiedType(
            fn->getReturnType().getCanonicalType(), returnType))
      candidates.push_back(fn);
  if (candidates.empty()) {
    eraseInProgress();
    return emitError(loc) << "unsupported: function pointer result type";
  }
  const clang::VarDecl *common = nullptr;
  for (const clang::FunctionDecl *fn : candidates) {
    FailureOr<Type> kind = classifyPointerReturn(fn, loc);
    if (failed(kind)) {
      eraseInProgress();
      return failure();
    }
    const clang::VarDecl *base =
        globalReturnBases.lookup(fn->getCanonicalDecl());
    if (*kind || !base) {
      eraseInProgress();
      return emitError(loc) << "unsupported: function pointer result type";
    }
    if (common && base != common) {
      eraseInProgress();
      return emitError(loc) << "unsupported: return sites disagree on the "
                               "returned global base";
    }
    common = base;
  }
  eraseInProgress();
  fnPtrReturnBases[key] = common;
  return common;
}

const clang::VarDecl *
CImporter::erasedGlobalReturnCallBase(const clang::Expr *expr,
                                      const clang::CallExpr **callOut) const {
  const auto *call = llvm::dyn_cast<clang::CallExpr>(stripTrivia(expr));
  if (!call)
    return nullptr;
  if (callOut)
    *callOut = call;
  if (const clang::FunctionDecl *callee = call->getDirectCallee())
    return globalReturnBases.lookup(callee->getCanonicalDecl());
  const clang::Expr *calleeExpr = call->getCallee()->IgnoreParenImpCasts();
  clang::QualType calleeType = calleeExpr->getType().getCanonicalType();
  if (!calleeType->isFunctionPointerType())
    return nullptr;
  return fnPtrReturnBases.lookup(
      calleeType->getPointeeType().getCanonicalType().getTypePtr());
}

bool CImporter::isCarrierReturnFunction(const clang::FunctionDecl *func) {
  const clang::FunctionDecl *canonical = func->getCanonicalDecl();
  auto it = carrierReturnCache.find(canonical);
  if (it != carrierReturnCache.end())
    return it->second;
  if (!isDataPointer(func->getReturnType())) {
    carrierReturnCache.try_emplace(canonical, false);
    return false;
  }
  const clang::FunctionDecl *definition = func->getDefinition();
  if (!definition || !definition->hasBody()) {
    carrierReturnCache.try_emplace(canonical, false);
    return false;
  }
  // Break recursion cycles pessimistically (a self-recursive carrier
  // return would need a fixpoint; none of the supported programs do).
  if (!carrierReturnInProgress.insert(canonical).second)
    return false;
  PointerRegionAnalysis regions;
  regions.carrierReturnQuery = [this](const clang::FunctionDecl *callee) {
    return isCarrierReturnFunction(callee);
  };
  regions.literalTemps = &literalTemps;
  regions.analyze(astContext(), definition->getBody());
  SmallVector<const clang::ReturnStmt *> returns;
  collectReturnStmts(definition->getBody(), returns);
  bool carrier = !returns.empty();
  for (const clang::ReturnStmt *ret : returns)
    if (!ret->getRetValue() ||
        !isCarrierReturnExpr(regions, ret->getRetValue())) {
      carrier = false;
      break;
    }
  carrierReturnInProgress.erase(canonical);
  carrierReturnCache.try_emplace(canonical, carrier);
  return carrier;
}

bool CImporter::isCarrierReturnExpr(PointerRegionAnalysis &regions,
                                    const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  if (e->isNullPointerConstant(astContext(),
                               clang::Expr::NPC_NeverValueDependent) !=
      clang::Expr::NPCK_NotNull)
    return true;
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
    if (cast->getCastKind() == clang::CK_NullToPointer)
      return true;
    // Only a pointer-width integer rides as a carrier (mirroring
    // `recordPointerWrite`'s classification gate).
    if (cast->getCastKind() == clang::CK_IntegralToPointer)
      return astContext().getTypeSize(cast->getSubExpr()->getType()) == 64;
    if (cast->getCastKind() == clang::CK_NoOp)
      return isCarrierReturnExpr(regions, cast->getSubExpr());
  }
  if (const clang::VarDecl *var = asLoadedLocalVarRef(e))
    if (regions.tracks(var) && isCarrierRegion(regions.regionOf(var)))
      return true;
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e))
    if (const clang::FunctionDecl *callee = call->getDirectCallee())
      return isCarrierReturnFunction(callee);
  return false;
}


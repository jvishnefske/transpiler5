//===- ImportCHosted.cpp - hosted libc call emission ------------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// CImporter's hosted-libc call emission: the <stdio.h> FILE* stream family
/// (fileHandleType/requestFileHelper/emitFileLocal/emitFileOpenInto/
/// emitFileHandleArg/emitFileGetc/emitFileReadWrite/emitFileGetsIndex/
/// emitFileClose/emitFileTruth), the string.h family
/// (emitCharRegionSlice/requestStringHelper/emitStringCopyCall/
/// emitMemsetCall/emitMemcpyCall/emitStrchrIndex), and the small
/// stdlib.h/math.h/putchar family (emitPutchar/emitHostedMathCall/
/// emitAbsCall/emitAtoiCall/emitExitCall). Split out of ImportC.cpp by pure
/// code motion (W1.11); see CImporterInternal.h for the CImporter class
/// declaration this file implements.
//
//===----------------------------------------------------------------------===//

#include "CImporterInternal.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// Hosted <stdio.h> FILE* streams (design.md C99-48, CTS-T1.3, 00187)
//===----------------------------------------------------------------------===//

emitrust::OpaqueType CImporter::fileHandleType() {
  return emitrust::OpaqueType::get(builder.getContext(), "__EmitrustFile");
}

void CImporter::requestFileHelper(llvm::StringRef name) {
  // Every helper mentions the handle enum, and the byte-loop helpers call
  // the fgetc primitive.
  neededFileHelpers.insert("__EmitrustFile");
  if (name == "__emitrust_fread" || name == "__emitrust_fgets")
    neededFileHelpers.insert("__emitrust_fgetc");
  neededFileHelpers.insert(name);
}

void CImporter::requestPoolHelpers() { neededPoolHelpers = true; }

LogicalResult CImporter::emitFileLocal(const clang::VarDecl *var,
                                       Location loc) {
  // The owned handle lives in an `emitrust.variable` place; without an
  // initializer it renders as `__EmitrustFile::Null` (C's NULL), so the
  // enum definition is needed as soon as a handle local exists.
  requestFileHelper("__EmitrustFile");
  // FR-61e: the FILE* handle local carries the local's final spelling.
  Value place = createVariablePlace(
      loc, fileHandleType(),
      var->getName().empty() ? std::string()
                             : mangleMemberName(var->getName()));
  fileLocals[var] = place;
  if (const clang::Expr *init = var->getInit())
    return emitFileOpenInto(place, init);
  return success();
}

LogicalResult CImporter::emitFileOpenInto(Value place,
                                          const clang::Expr *init) {
  const clang::Expr *e = init->IgnoreParenImpCasts();
  Location loc = translateLoc(e->getBeginLoc());
  const auto *call = llvm::dyn_cast<clang::CallExpr>(e);
  const clang::FunctionDecl *callee =
      call ? call->getDirectCallee() : nullptr;
  if (!callee || !callee->getDeclName().isIdentifier() ||
      callee->getName() != "fopen" || callee->getDefinition())
    return emitError(loc) << "unsupported: a FILE* local may only be "
                             "initialized or assigned by fopen";
  if (call->getNumArgs() != 2)
    return emitError(loc)
           << "unsupported: fopen requires a path and a mode";
  // The mode selects the helper. Exactly "r" and "w" are in the slice:
  // append/update modes and the (POSIX no-op) binary suffix stay out.
  const auto *modeLiteral = llvm::dyn_cast<clang::StringLiteral>(
      call->getArg(1)->IgnoreParenImpCasts());
  if (!modeLiteral || !modeLiteral->isOrdinary())
    return emitError(translateLoc(call->getArg(1)->getBeginLoc()))
           << "unsupported: fopen mode must be a string literal";
  llvm::StringRef mode = modeLiteral->getString();
  if (mode != "r" && mode != "w")
    return emitError(translateLoc(call->getArg(1)->getBeginLoc()))
           << "unsupported: fopen mode \"" << mode
           << "\" (only \"r\" and \"w\" are supported)";
  // Literal-only paths (v1): the decoded bytes are re-emitted as a Rust
  // string literal, so they must be printable ASCII (an embedded NUL
  // would also diverge from C's NUL-terminated path).
  const auto *pathLiteral = llvm::dyn_cast<clang::StringLiteral>(
      call->getArg(0)->IgnoreParenImpCasts());
  Location pathLoc = translateLoc(call->getArg(0)->getBeginLoc());
  if (!pathLiteral || !pathLiteral->isOrdinary())
    return emitError(pathLoc)
           << "unsupported: fopen path must be an ordinary string literal";
  llvm::StringRef path = pathLiteral->getString();
  for (char c : path)
    if (c < 0x20 || c > 0x7e)
      return emitError(pathLoc) << "unsupported: non-printable or "
                                   "non-ASCII byte in fopen path";
  llvm::StringRef helper =
      mode == "r" ? "__emitrust_fopen_r" : "__emitrust_fopen_w";
  requestFileHelper(helper);
  Value handle =
      builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{fileHandleType()}, builder.getStringAttr(helper),
              builder.getArrayAttr({builder.getStringAttr(path)}),
              ValueRange{})
          .getResult(0);
  builder.create<emitrust::AssignOp>(loc, place, handle);
  return success();
}

FailureOr<Value> CImporter::emitFileHandleArg(const clang::Expr *expr,
                                              bool isMut) {
  const clang::Expr *e = expr->IgnoreParenImpCasts();
  Location loc = translateLoc(e->getBeginLoc());
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
  const auto *var =
      ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
  // A FILE* parameter means the stream crossed into this function — the
  // v1 no-escape rule, pinned by the escape rejection case.
  if (var && llvm::isa<clang::ParmVarDecl>(var) &&
      isFilePtrType(var->getType()))
    return emitError(loc) << "unsupported: FILE* cannot cross a "
                             "user-defined function boundary";
  Value place = var ? fileLocals.lookup(var) : Value();
  if (!place)
    return emitError(loc) << "unsupported: a FILE* stream argument must be "
                             "a function-local FILE* variable";
  Type refType = isMut ? Type(emitrust::MutRefType::get(fileHandleType()))
                       : Type(emitrust::RefType::get(fileHandleType()));
  return builder.create<emitrust::AddrOfOp>(loc, refType, place, isMut)
      .getResult();
}

FailureOr<Value> CImporter::emitFileGetc(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: fgetc requires exactly one argument";
  FailureOr<Value> handle = emitFileHandleArg(call->getArg(0), /*isMut=*/true);
  if (failed(handle))
    return failure();
  requestFileHelper("__emitrust_fgetc");
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{builder.getI32Type()},
          builder.getStringAttr("__emitrust_fgetc"), /*args=*/ArrayAttr(),
          ValueRange{*handle})
      .getResult(0);
}

FailureOr<Value> CImporter::emitFileReadWrite(const clang::CallExpr *call,
                                              bool isWrite) {
  llvm::StringRef name = isWrite ? "fwrite" : "fread";
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 4)
    return emitError(loc) << "unsupported: " << name
                          << " requires four arguments";
  // Byte-wise only: the element size must be the integer constant 1. The
  // check precedes the buffer lowering so a wide element over a non-char
  // buffer still reports the pinned byte-wise wording.
  clang::Expr::EvalResult size;
  if (!call->getArg(1)->EvaluateAsInt(size, astContext()) ||
      size.Val.getInt() != 1)
    return emitError(loc)
           << "unsupported: " << name
           << " element size other than 1 (FILE* I/O is byte-wise)";
  // The count value materializes before the borrows below, so no load
  // intervenes between a borrow and the helper call consuming it.
  FailureOr<Value> count = emitRValue(call->getArg(2));
  if (failed(count))
    return failure();
  if (!llvm::isa<IntegerType>((*count).getType()))
    return emitError(loc) << "unsupported: " << name << " count type";
  Value countI64 = castToIntType(loc, *count, builder.getIntegerType(64));
  FailureOr<PtrExprValue> region = emitCharRegionArg(call->getArg(0));
  if (failed(region))
    return failure();
  // fread fills the destination (mutable borrow); fwrite only reads its
  // source, so a string-literal region is fine there.
  FailureOr<Value> slice = emitCharRegionSlice(loc, *region,
                                               /*isMut=*/!isWrite);
  if (failed(slice))
    return failure();
  FailureOr<Value> handle = emitFileHandleArg(call->getArg(3), /*isMut=*/true);
  if (failed(handle))
    return failure();
  llvm::StringRef helper =
      isWrite ? "__emitrust_fwrite" : "__emitrust_fread";
  requestFileHelper(helper);
  Value bytes = builder
                    .create<emitrust::CallOpaqueOp>(
                        loc, TypeRange{builder.getIntegerType(64)},
                        builder.getStringAttr(helper), /*args=*/ArrayAttr(),
                        ValueRange{*handle, *slice, countI64})
                    .getResult(0);
  // Convert the i64 byte count to the call's declared C result type
  // (size_t), matching emitStrlenCall's convention.
  FailureOr<Type> resultType = mapType(call->getType(), loc);
  if (failed(resultType))
    return failure();
  auto intType = llvm::dyn_cast<IntegerType>(*resultType);
  if (!intType)
    return emitError(loc) << "unsupported: " << name << " result type";
  return castToIntType(loc, bytes, intType);
}

FailureOr<Value> CImporter::emitFileGetsIndex(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 3)
    return emitError(loc) << "unsupported: fgets requires three arguments";
  // The size value materializes before the borrows (same discipline as
  // fread/fwrite).
  FailureOr<Value> sizeValue = emitRValue(call->getArg(1));
  if (failed(sizeValue))
    return failure();
  if (!llvm::isa<IntegerType>((*sizeValue).getType()))
    return emitError(loc) << "unsupported: fgets size type";
  Value sizeI64 =
      castToIntType(loc, *sizeValue, builder.getIntegerType(64));
  FailureOr<PtrExprValue> region = emitCharRegionArg(call->getArg(0));
  if (failed(region))
    return failure();
  FailureOr<Value> slice = emitCharRegionSlice(loc, *region, /*isMut=*/true);
  if (failed(slice))
    return failure();
  FailureOr<Value> handle = emitFileHandleArg(call->getArg(2), /*isMut=*/true);
  if (failed(handle))
    return failure();
  requestFileHelper("__emitrust_fgets");
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{builder.getIntegerType(64)},
          builder.getStringAttr("__emitrust_fgets"), /*args=*/ArrayAttr(),
          ValueRange{*handle, *slice, sizeI64})
      .getResult(0);
}

const clang::CallExpr *
CImporter::asHostedFgetsCall(const clang::Expr *expr) const {
  const auto *call =
      llvm::dyn_cast<clang::CallExpr>(expr->IgnoreParenImpCasts());
  const clang::FunctionDecl *callee =
      call ? call->getDirectCallee() : nullptr;
  if (!callee || !callee->getDeclName().isIdentifier() ||
      callee->getName() != "fgets" || callee->getDefinition())
    return nullptr;
  return call;
}

LogicalResult CImporter::emitFileClose(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: fclose requires exactly one argument";
  FailureOr<Value> handle = emitFileHandleArg(call->getArg(0), /*isMut=*/true);
  if (failed(handle))
    return failure();
  requestFileHelper("__emitrust_fclose");
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("__emitrust_fclose"),
      /*args=*/ArrayAttr(), ValueRange{*handle});
  return success();
}

FailureOr<Value> CImporter::emitFileTruth(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParenImpCasts();
  Location loc = translateLoc(e->getBeginLoc());
  // A truth-tested fgets result asks "did a line arrive": the helper's
  // -1 is C's NULL, so the test is an index comparison (the strchr
  // convention).
  if (const clang::CallExpr *gets = asHostedFgetsCall(e)) {
    FailureOr<Value> index = emitFileGetsIndex(gets);
    if (failed(index))
      return failure();
    Value nullIndex =
        createIntConstant(loc, builder.getIntegerType(64), -1);
    return builder
        .create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne, *index,
                               nullIndex)
        .getResult();
  }
  // A handle's truth is its NULL check, read through a shared borrow.
  FailureOr<Value> handle = emitFileHandleArg(e, /*isMut=*/false);
  if (failed(handle))
    return failure();
  requestFileHelper("__emitrust_file_ok");
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{builder.getI1Type()},
          builder.getStringAttr("__emitrust_file_ok"), /*args=*/ArrayAttr(),
          ValueRange{*handle})
      .getResult(0);
}

FailureOr<PtrExprValue>
CImporter::emitCharRegionArg(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  Location loc = translateLoc(e->getBeginLoc());
  // The void* parameters of memset/memcpy/memcmp wrap their arguments in
  // implicit pointer bitcasts, and const-qualified parameters (strcpy's
  // source, ...) in no-op qualification casts; the region underneath is
  // byte-typed either way, so both cast kinds are stripped.
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    if ((cast->getCastKind() != clang::CK_BitCast &&
         cast->getCastKind() != clang::CK_NoOp) ||
        !isPointerType(cast->getType()))
      break;
    e = stripTrivia(cast->getSubExpr());
  }
  // FR-88: a NULLABLE byte-slice parameter used as a byte-family region.
  // The guard-dominance scan already proved every such use sits under a
  // proven null guard, so the value is unwrapped AT the use site — the
  // panic-free placement: `p.unwrap()` sits inside the guard by
  // construction (MethodCallOp is non-Pure, so no canonicalization can
  // hoist it out) — and the deref'd `&[u8]` slice place rides the
  // ordinary FR-72 reslice machinery through `slicePlace`.
  if (const clang::ParmVarDecl *param = asPointerParamRef(e);
      param && nullableByteParams.contains(param)) {
    auto sliceType = emitrust::SliceType::get(
        IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned));
    Value place = symbols.lookup(param);
    Value unwrapped =
        builder
            .create<emitrust::MethodCallOp>(
                loc, TypeRange{emitrust::RefType::get(sliceType)}, place,
                builder.getStringAttr("unwrap"), ValueRange{})
            .getResult(0);
    Value slicePlace =
        builder
            .create<emitrust::DerefOp>(
                loc, emitrust::LValueType::get(sliceType), unwrapped)
            .getResult();
    PtrExprValue value{
        nullptr, createIntConstant(loc, builder.getIntegerType(64), 0),
        Value()};
    value.slicePlace = slicePlace;
    return value;
  }
  // A decayed string literal argument — or `__func__`, whose
  // function-name literal is the same shape (C99-29) — creates (or
  // reuses) the literal's read-only backing; unlike a literal bound to a
  // pointer variable, this shape may appear with no pointer region
  // referring to the literal.
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_ArrayToPointerDecay)
      if (const clang::StringLiteral *literal =
              underlyingStringLiteral(stripTrivia(cast->getSubExpr()))) {
        FailureOr<Value> backing = getOrCreateLiteralBacking(literal, loc);
        if (failed(backing))
          return failure();
        return PtrExprValue{
            nullptr, createIntConstant(loc, builder.getIntegerType(64), 0),
            *backing};
      }
  FailureOr<PtrExprValue> pointer = emitPointerRValue(e);
  if (failed(pointer))
    return failure();
  // A multi-base pointer has no single char region to borrow (CTS-P7
  // scope).
  if (pointer->baseIndex)
    return emitError(loc) << "unsupported: passing a pointer bound to "
                             "multiple objects to a string function";
  if (!pointer->cursor)
    return emitError(loc) << "unsupported: the address of a scalar object "
                             "is not a string region";
  return *pointer;
}

/// The element type under a borrowed char-region slice
/// (`!emitrust.ref/mut_ref<!emitrust.slice<T>>`), used by the byte-family
/// callers to keep the emitted helper's signature in agreement with the
/// call site (FR-72): the helper images are i8- or u8-typed, and
/// CallOpaqueOp is untyped at translate time, so a disagreement would
/// surface only as a rustc E0308 in the emitted crate.
static Type charRegionElement(Value slice) {
  Type pointee;
  if (auto mutRef = llvm::dyn_cast<emitrust::MutRefType>(slice.getType()))
    pointee = mutRef.getPointee();
  else if (auto ref = llvm::dyn_cast<emitrust::RefType>(slice.getType()))
    pointee = ref.getPointee();
  return llvm::cast<emitrust::SliceType>(pointee).getElementType();
}

/// Whether the borrowed char-region slice is the unsigned (ui8/u8)
/// element domain — the `uint8_t *` parameter convention — which selects
/// the parallel `__emitrust_*_u8` helper images.
static bool isUnsignedByteRegion(Value slice) {
  auto intType = llvm::dyn_cast<IntegerType>(charRegionElement(slice));
  return intType && intType.isUnsigned();
}

/// FR-97 (generalizing FR-87's u32 subset): the byte width of a TYPED
/// integer element admitted for the gated byte-splat memset image
/// (i16/ui16/i32/ui32/i64/ui64), or nullopt outside the map. The byte
/// elements (i8/ui8) ride the historical byte helpers and are
/// deliberately excluded.
static std::optional<unsigned> typedSplatWidthBytes(Type element) {
  auto intType = llvm::dyn_cast<IntegerType>(element);
  // The importer spells C's signed integers signless and its unsigned
  // ones explicitly Unsigned; an explicitly Signed type never occurs.
  if (!intType || intType.isSigned())
    return std::nullopt;
  switch (intType.getWidth()) {
  case 16:
  case 32:
  case 64:
    return intType.getWidth() / 8;
  default:
    return std::nullopt;
  }
}

/// The diagnostic spelling of a typed integer region: the FR-87 u32
/// wording ("an unsigned int region") is pinned verbatim by the FR-87
/// frontier tests; every other mapped width shares one generalized
/// spelling.
static llvm::StringRef typedRegionDesc(Type element) {
  auto intType = llvm::cast<IntegerType>(element);
  return intType.getWidth() == 32 && intType.isUnsigned()
             ? "an unsigned int region"
             : "a typed integer region";
}

/// The per-width word-fill helper image for a mapped splat element (see
/// kStringHelpers in ImportCFunctions.cpp).
static llvm::StringRef typedSplatHelper(Type element) {
  auto intType = llvm::cast<IntegerType>(element);
  bool isUnsignedElement = intType.isUnsigned();
  switch (intType.getWidth()) {
  case 16:
    return isUnsignedElement ? "__emitrust_memset_u16"
                             : "__emitrust_memset_i16";
  case 32:
    return isUnsignedElement ? "__emitrust_memset_u32"
                             : "__emitrust_memset_i32";
  default:
    return isUnsignedElement ? "__emitrust_memset_u64"
                             : "__emitrust_memset_i64";
  }
}

/// Whether a resolved i64 element cursor is the constant 0 — the
/// whole-array decay `memset(a, ...)`. FR-97 admits only whole LOCAL
/// arrays; an offset destination (`memset(a + 1, ...)`) is unexercised
/// by the motivating corpus and keeps the historical located rejection
/// this wave.
static bool isConstantZeroIndex(Value cursor) {
  if (!cursor)
    return false;
  auto constant = cursor.getDefiningOp<arith::ConstantOp>();
  if (!constant)
    return false;
  auto attr = llvm::dyn_cast<IntegerAttr>(constant.getValue());
  return attr && attr.getValue().isZero();
}

FailureOr<Value> CImporter::emitCharRegionSlice(Location loc,
                                                const PtrExprValue &pointer,
                                                bool isMut,
                                                bool allowUnsignedByte) {
  Value place = pointer.literalBacking;
  if (place && isMut)
    return emitError(loc) << "unsupported: a string literal region cannot "
                             "be a mutable string argument";
  // FR-88: a nullable parameter's guarded use carries its unwrapped
  // `!emitrust.lvalue<!emitrust.slice<ui8>>` place directly; it joins
  // the slice-param branch below (shared-source rules included) exactly
  // like the FR-72 deref'd backing it is.
  if (!place)
    place = pointer.slicePlace;
  // FR-146: a HEAP-ALLOCATION region (W4.2e Part A) borrows exactly like
  // a local array region. Its backing is a synthesized entry-block
  // MUTABLE `!emitrust.lvalue<!emitrust.array<CAP x T>>` local — the same
  // place shape a `char a[N]` region resolves to, and writable, so both
  // the shared and the mutable borrow are legal — subscripted at the
  // pointer's own cursor. A node-pool handle's backing is an
  // `emitrust.collection` (`!emitrust.lvalue<!emitrust.opaque<
  // "__emitrust_collection">>`) instead: it is not an array place, so it
  // declines to the char-array rejection below rather than being
  // borrowed. Before this branch existed an allocation-backed pointer
  // fell through to the base lookup with a NULL `base` and crashed
  // dereferencing it (FR-146).
  if (!place)
    place = pointer.backing;
  if (!place && !pointer.base)
    return emitError(loc) << "unsupported: string function argument over a "
                             "pointer with no importable region";
  if (!place) {
    auto it = symbols.find(pointer.base);
    if (it == symbols.end())
      return emitError(loc)
             << "unsupported: pointer target '" << pointer.base->getName()
             << "' is not an importable place";
    place = it->second;
  }
  // Every region place below is subscripted at the pointer's cursor; a
  // degenerate (cursor-less) pointer has no element to start at.
  // (Defensive: `emitCharRegionArg` already rejects that shape.)
  if (!pointer.cursor)
    return emitError(loc) << "unsupported: string function argument over a "
                             "pointer with no region cursor";
  auto lvalueType = llvm::dyn_cast<emitrust::LValueType>(place.getType());
  Type i8Type = builder.getIntegerType(8);
  Type ui8Type =
      IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  Type elementType;
  bool isSliceParamRegion = false;
  if (lvalueType) {
    if (auto arrayType =
            llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType())) {
      // FR-88: a ui8 (uint8_t) LOCAL/staged array is a byte-family
      // region too — the ctr_prng `memcpy(personalization_buf, ...)`
      // destination — under the same u8-helper gate the FR-72
      // slice-param branch uses; str*-family positions
      // (allowUnsignedByte=false) keep the historical char-array
      // rejection wording verbatim.
      if (arrayType.getElementType() == i8Type ||
          (allowUnsignedByte && arrayType.getElementType() == ui8Type))
        elementType = arrayType.getElementType();
    } else if (auto sliceBase = llvm::dyn_cast<emitrust::SliceType>(
                   lvalueType.getValueType())) {
      // FR-72: a byte-slice PARAMETER region (the (deref'd backing,
      // cursor) decomposition of a `char */uint8_t *` — or FR-71-admitted
      // `void *` — parameter) reslices at the argument's cursor exactly
      // like the general slice-param call machinery; the slice's own
      // length keeps every helper access bounds-checked.
      if (sliceBase.getElementType() == i8Type ||
          sliceBase.getElementType() == ui8Type) {
        elementType = sliceBase.getElementType();
        isSliceParamRegion = true;
      }
    }
  }
  if (!elementType)
    return emitError(loc) << "unsupported: string function argument must "
                             "designate a char array";
  if (isSliceParamRegion) {
    // The str*-family/strchr/atoi/strlen/sprintf helpers are i8-typed;
    // only the byte-family callers (memset/memcpy/memmove/memcmp) carry
    // u8 helper images, so a ui8 region anywhere else rejects here
    // instead of failing element typing only at rustc (E0308).
    if (elementType == ui8Type && !allowUnsignedByte)
      return emitError(loc) << "unsupported: string function argument over "
                               "an unsigned char region";
    // A shared (const-pointee) slice parameter can only be a SOURCE
    // region: the SliceOfOp verifier and translate both accept a `mut`
    // reslice of it, so without this check the emitted `&mut (*p)[..]`
    // would fail only at rustc (E0596).
    if (isMut) {
      auto deref = place.getDefiningOp<emitrust::DerefOp>();
      if (!deref ||
          !llvm::isa<emitrust::MutRefType>(deref.getOperand().getType()))
        return emitError(loc) << "unsupported: a shared byte-slice "
                                 "parameter cannot be a mutable string "
                                 "argument";
    }
  }
  auto sliceType = emitrust::SliceType::get(elementType);
  Type refType = isMut ? Type(emitrust::MutRefType::get(sliceType))
                       : Type(emitrust::RefType::get(sliceType));
  return builder
      .create<emitrust::SliceOfOp>(loc, refType, place, pointer.cursor,
                                   isMut)
      .getResult();
}

FailureOr<CharRegionArg>
CImporter::emitByteRegionArg(const clang::Expr *expr, bool isMut,
                             bool interceptMember) {
  // FR-87: the member-array shapes FR-74/86 admit as slice ARGUMENTS
  // resolve as byte-family REGIONS too, through the same interception
  // core, BEFORE the pointer decomposition (which has no member-rooted
  // representation and would reject with the historical decay wording).
  // The strip mirrors `emitCharRegionArg`: the void* parameters of
  // memset/memcpy/memcmp wrap their arguments in implicit pointer
  // bitcasts (and const-qualified parameters in no-op casts).
  if (interceptMember) {
    const clang::Expr *e = stripTrivia(expr);
    while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
      if ((cast->getCastKind() != clang::CK_BitCast &&
           cast->getCastKind() != clang::CK_NoOp) ||
          !isPointerType(cast->getType()))
        break;
      e = stripTrivia(cast->getSubExpr());
    }
    Location loc = translateLoc(e->getBeginLoc());
    CharRegionArg arg;
    FailureOr<bool> matched = tryEmitMemberArraySlicePlace(
        loc, e, isMut, arg.memberPlace, arg.memberCursor, arg.memberElement,
        arg.memberRoot, arg.memberPath);
    if (failed(matched))
      return failure();
    Type i8Type = builder.getIntegerType(8);
    Type ui8Type =
        IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
    // Only the elements with an admitted (or explicitly gated: the
    // FR-97 typed-integer widths are memset-destination-only) helper
    // image take the member channel; every other element declines so
    // the historical located rejection stays verbatim.
    if (*matched && (arg.memberElement == i8Type ||
                     arg.memberElement == ui8Type ||
                     typedSplatWidthBytes(arg.memberElement)))
      return arg;
  }
  FailureOr<PtrExprValue> pointer = emitCharRegionArg(expr);
  if (failed(pointer))
    return failure();
  CharRegionArg arg;
  arg.pointer = *pointer;
  return arg;
}

FailureOr<Value> CImporter::emitByteRegionSlice(Location loc,
                                                const CharRegionArg &arg,
                                                bool isMut,
                                                bool allowUnsignedByte) {
  if (!arg.isMember())
    return emitCharRegionSlice(loc, arg.pointer, isMut, allowUnsignedByte);
  Type i8Type = builder.getIntegerType(8);
  Type ui8Type =
      IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  // FR-87/97 typed gate: the only admitted typed-integer use is the
  // memset destination, which `emitMemsetCall` lowers itself (word-fill
  // image) before ever reaching here — every other byte-family (and
  // str*-family) position rejects located, mirroring FR-72's
  // unsigned-char-region wording (the FR-87 u32 spelling verbatim; the
  // FR-97 widths share the generalized spelling).
  if (arg.memberElement != i8Type && arg.memberElement != ui8Type)
    return emitError(loc) << "unsupported: string function argument over "
                          << typedRegionDesc(arg.memberElement);
  if (arg.memberElement == ui8Type && !allowUnsignedByte)
    return emitError(loc) << "unsupported: string function argument over "
                             "an unsigned char region";
  auto sliceType = emitrust::SliceType::get(arg.memberElement);
  Type refType = isMut ? Type(emitrust::MutRefType::get(sliceType))
                       : Type(emitrust::RefType::get(sliceType));
  return builder
      .create<emitrust::SliceOfOp>(loc, refType, arg.memberPlace,
                                   arg.memberCursor, isMut)
      .getResult();
}

void CImporter::requestStringHelper(llvm::StringRef name) {
  neededStringHelpers.insert(name);
}

LogicalResult CImporter::emitStringCopyCall(const clang::CallExpr *call,
                                            llvm::StringRef name,
                                            bool hasCount) {
  Location loc = translateLoc(call->getBeginLoc());
  unsigned expected = hasCount ? 3 : 2;
  if (call->getNumArgs() != expected)
    return emitError(loc) << "unsupported: " << name << " requires exactly "
                          << expected << " arguments";
  FailureOr<PtrExprValue> dst = emitCharRegionArg(call->getArg(0));
  if (failed(dst))
    return failure();
  FailureOr<PtrExprValue> src = emitCharRegionArg(call->getArg(1));
  if (failed(src))
    return failure();
  if (dst->base && dst->base == src->base)
    return emitError(loc)
           << "unsupported: " << name
           << " source and destination point into the same object '"
           << dst->base->getName() << "'";
  // FR-146: two regions of the SAME heap allocation have no named base to
  // collide on — their shared identity is the synthesized backing place.
  // Borrowing it mutably and shared at once is rustc E0502, so the pair
  // rejects here rather than after emission.
  if (dst->backing && dst->backing == src->backing)
    return emitError(loc)
           << "unsupported: " << name
           << " source and destination point into the same allocation";
  Value count;
  if (hasCount) {
    FailureOr<Value> n = emitRValue(call->getArg(2));
    if (failed(n))
      return failure();
    if (!llvm::isa<IntegerType>((*n).getType()))
      return emitError(loc) << "unsupported: " << name << " count type";
    count = castToIntType(loc, *n, builder.getIntegerType(64));
  }
  // The mutable destination borrow and the shared source borrow are
  // created back to back, immediately before the call: no load of either
  // base intervenes, so rustc accepts the pair.
  FailureOr<Value> dstSlice = emitCharRegionSlice(loc, *dst, /*isMut=*/true);
  if (failed(dstSlice))
    return failure();
  FailureOr<Value> srcSlice =
      emitCharRegionSlice(loc, *src, /*isMut=*/false);
  if (failed(srcSlice))
    return failure();
  SmallVector<Value> operands{*dstSlice, *srcSlice};
  if (count)
    operands.push_back(count);
  requestStringHelper(("__emitrust_" + name).str());
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(),
      builder.getStringAttr(("__emitrust_" + name).str()),
      /*args=*/ArrayAttr(), operands);
  return success();
}

LogicalResult CImporter::emitMemsetCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 3)
    return emitError(loc)
           << "unsupported: memset requires exactly 3 arguments";
  FailureOr<CharRegionArg> dst = emitByteRegionArg(
      call->getArg(0), /*isMut=*/true, /*interceptMember=*/true);
  if (failed(dst))
    return failure();
  FailureOr<Value> byte = emitRValue(call->getArg(1));
  if (failed(byte))
    return failure();
  FailureOr<Value> n = emitRValue(call->getArg(2));
  if (failed(n))
    return failure();
  if (!llvm::isa<IntegerType>((*byte).getType()) ||
      !llvm::isa<IntegerType>((*n).getType()))
    return emitError(loc) << "unsupported: memset argument type";
  Value fill = castToIntType(loc, *byte, builder.getI32Type());
  Value count = castToIntType(loc, *n, builder.getIntegerType(64));
  // FR-97 (generalizing FR-87's u32 member subset): a TYPED integer
  // array as the memset DESTINATION is byte-fillable exactly — memset
  // semantics are bytes — when the byte count provably covers whole
  // elements (compile-time constant, multiple of the element size) and
  // the fill byte is a compile-time constant, so the fill WORD is its
  // per-width replication b * 0x0101 / 0x01010101 / 0x0101010101010101
  // (endianness-neutral: all bytes equal; the FR-87 spike's byte-diff
  // caught a hand-picked word constant here — 0xAB is the proof byte).
  // Destinations: FR-87's MEMBER regions at every mapped width, and —
  // FR-97 — a whole LOCAL typed array (constant-zero cursor; offset
  // destinations keep the historical located rejection this wave). The
  // element cursor and the byte count pass through to the per-width
  // `__emitrust_memset_*` word walker unchanged. Everything inside the
  // typed subset but outside the gates rejects located.
  Type splatElement;
  Value splatPlace;
  Value splatCursor;
  if (dst->isMember()) {
    if (typedSplatWidthBytes(dst->memberElement)) {
      splatElement = dst->memberElement;
      splatPlace = dst->memberPlace;
      splatCursor = dst->memberCursor;
    }
  } else if (const PtrExprValue &ptr = dst->pointer;
             ptr.base && !ptr.literalBacking && !ptr.slicePlace &&
             !ptr.backing && !ptr.nonNull && !ptr.baseIndex &&
             ptr.multiBases.empty() && !ptr.member &&
             isConstantZeroIndex(ptr.cursor)) {
    // The LOCAL typed-array destination: the same place resolution
    // `emitCharRegionSlice` uses (which would reject every non-byte
    // element with the char-array wording), restricted to the plain
    // single-base whole-object pointer shape.
    auto it = symbols.find(ptr.base);
    if (it != symbols.end()) {
      if (auto lvalueType =
              llvm::dyn_cast<emitrust::LValueType>(it->second.getType()))
        if (auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(
                lvalueType.getValueType()))
          if (typedSplatWidthBytes(arrayType.getElementType())) {
            splatElement = arrayType.getElementType();
            splatPlace = it->second;
            splatCursor = ptr.cursor;
          }
    }
  }
  if (splatElement) {
    int64_t elemBytes = *typedSplatWidthBytes(splatElement);
    llvm::StringRef desc = typedRegionDesc(splatElement);
    std::optional<llvm::APSInt> constCount =
        call->getArg(2)->getIntegerConstantExpr(astContext());
    if (!constCount || constCount->getExtValue() % elemBytes != 0)
      return emitError(loc)
             << "unsupported: memset over " << desc
             << " requires a constant byte count that is a multiple of "
             << elemBytes;
    std::optional<llvm::APSInt> constFill =
        call->getArg(1)->getIntegerConstantExpr(astContext());
    if (!constFill)
      return emitError(loc) << "unsupported: memset over " << desc
                            << " requires a constant fill byte";
    uint64_t fillByte =
        static_cast<uint64_t>(constFill->getExtValue()) & 0xFFu;
    uint64_t pattern = elemBytes == 2   ? 0x0101u
                       : elemBytes == 4 ? 0x01010101u
                                        : 0x0101010101010101u;
    uint64_t fillWord = fillByte * pattern;
    // Signed elements carry the word's sign-extended bit pattern
    // (0xFF -> -1i16; 0xAB -> -21589i16 / -1414812757i32); unsigned
    // ones its zero-extended value (0xFF over ui64 is the exact
    // all-ones pattern, u64::MAX).
    int64_t fillValue =
        llvm::cast<IntegerType>(splatElement).isUnsigned()
            ? static_cast<int64_t>(fillWord)
            : llvm::APInt(elemBytes * 8, fillWord).getSExtValue();
    auto sliceType = emitrust::SliceType::get(splatElement);
    Value slice = builder
                      .create<emitrust::SliceOfOp>(
                          loc, emitrust::MutRefType::get(sliceType),
                          splatPlace, splatCursor, /*is_mut=*/true)
                      .getResult();
    Value word = createScalarIntConstant(loc, splatElement, fillValue);
    llvm::StringRef helper = typedSplatHelper(splatElement);
    requestStringHelper(helper);
    builder.create<emitrust::CallOpaqueOp>(
        loc, TypeRange(), builder.getStringAttr(helper),
        /*args=*/ArrayAttr(), ValueRange{slice, word, count});
    return success();
  }
  FailureOr<Value> dstSlice = emitByteRegionSlice(loc, *dst, /*isMut=*/true,
                                                  /*allowUnsignedByte=*/true);
  if (failed(dstSlice))
    return failure();
  // FR-72 element agreement: a ui8 (uint8_t-parameter) region takes the
  // u8 helper image; an i8 region keeps the historical helper.
  llvm::StringRef helper = isUnsignedByteRegion(*dstSlice)
                               ? "__emitrust_memset_u8"
                               : "__emitrust_memset";
  requestStringHelper(helper);
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr(helper),
      /*args=*/ArrayAttr(), ValueRange{*dstSlice, fill, count});
  return success();
}

LogicalResult CImporter::emitMemcpyCall(const clang::CallExpr *call,
                                        llvm::StringRef name) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 3)
    return emitError(loc)
           << "unsupported: " << name << " requires exactly 3 arguments";
  FailureOr<CharRegionArg> dst = emitByteRegionArg(
      call->getArg(0), /*isMut=*/true, /*interceptMember=*/true);
  if (failed(dst))
    return failure();
  // FR-93: a MULTI-BASE pointer source (the aes tail
  // `memcpy(ctx->Iv, Iv, 16)`) has no single region to borrow; the copy
  // dispatches per source base instead. Intercepted BEFORE the source's
  // region resolution, whose pointer channel rejects any multi-base
  // read.
  if (const clang::VarDecl *multiSrc =
          astContext().getLangOpts().CPlusPlus
              ? nullptr
              : asMultiBasePointerRead(call->getArg(1)))
    return emitMultiBaseMemcpy(call, name, *dst, multiSrc, loc);
  FailureOr<CharRegionArg> src = emitByteRegionArg(
      call->getArg(1), /*isMut=*/false, /*interceptMember=*/true);
  if (failed(src))
    return failure();
  FailureOr<Value> n = emitRValue(call->getArg(2));
  if (failed(n))
    return failure();
  if (!llvm::isa<IntegerType>((*n).getType()))
    return emitError(loc) << "unsupported: " << name << " count type";
  Value count = castToIntType(loc, *n, builder.getIntegerType(64));
  // FR-87/97: the typed-integer member subset is memset-destination-only
  // — no memcpy/memmove word image is admitted, so either typed member
  // region rejects here with the same wording the slice gate uses
  // (before the same-root branch below could ever select a byte-typed
  // copy_within image for it).
  Type wallI8Type = builder.getIntegerType(8);
  Type wallUi8Type =
      IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  auto isTypedMember = [&](const CharRegionArg &arg) {
    return arg.isMember() && arg.memberElement != wallI8Type &&
           arg.memberElement != wallUi8Type;
  };
  if (isTypedMember(*dst) || isTypedMember(*src))
    return emitError(loc) << "unsupported: string function argument over "
                          << typedRegionDesc(isTypedMember(*dst)
                                                 ? dst->memberElement
                                                 : src->memberElement);
  // FR-74/87 aliasing key: a member region borrows only its FIELD, so
  // the collision key is (root, field-path). Member paths always end at
  // an ARRAY leaf, so two DISTINCT paths of one root can never prefix-
  // overlap — they are disjoint fields and the two-borrow emission below
  // is legal Rust — while the SAME path is a provable whole region and
  // rides `copy_within` on the member place (memmove's overlap-correct
  // semantics, refining C's undefined overlapping memcpy; the naive
  // two-borrow form is rustc E0502). A member/non-member mix on one
  // root has no provable relation and rejects.
  const clang::VarDecl *dstRoot =
      dst->isMember() ? dst->memberRoot : dst->pointer.base;
  const clang::VarDecl *srcRoot =
      src->isMember() ? src->memberRoot : src->pointer.base;
  if (dstRoot && dstRoot == srcRoot) {
    if (dst->isMember() != src->isMember())
      return emitError(loc)
             << "unsupported: " << name
             << " source and destination point into the same object '"
             << dstRoot->getName() << "'";
    if (dst->isMember()) {
      if (dst->memberPath == src->memberPath) {
        Type ui8Type = IntegerType::get(builder.getContext(), 8,
                                        IntegerType::Unsigned);
        auto sliceType = emitrust::SliceType::get(dst->memberElement);
        // The WHOLE member array is borrowed (cursor 0): the helper's
        // dst/src cursors are absolute element offsets into the member,
        // so a slice starting at the dst cursor would shift the copy
        // window (the spike-mandated byte-diff caught exactly that).
        Value zero =
            createIntConstant(loc, builder.getIntegerType(64), 0);
        Value slice = builder
                          .create<emitrust::SliceOfOp>(
                              loc, emitrust::MutRefType::get(sliceType),
                              dst->memberPlace, zero,
                              /*is_mut=*/true)
                          .getResult();
        llvm::StringRef helper = dst->memberElement == ui8Type
                                     ? "__emitrust_memcpy_within_u8"
                                     : "__emitrust_memcpy_within";
        requestStringHelper(helper);
        builder.create<emitrust::CallOpaqueOp>(
            loc, TypeRange(), builder.getStringAttr(helper),
            /*args=*/ArrayAttr(),
            ValueRange{slice, dst->memberCursor, src->memberCursor, count});
        return success();
      }
      // Distinct member paths of one root: disjoint fields, admitted
      // below. (Defensive: a proper prefix would be a matcher bug —
      // array leaves have no fields — but reject rather than emit.)
      size_t common =
          std::min(dst->memberPath.size(), src->memberPath.size());
      if (llvm::ArrayRef(dst->memberPath).take_front(common) ==
          llvm::ArrayRef(src->memberPath).take_front(common))
        return emitError(loc)
               << "unsupported: " << name
               << " source and destination point into the same object '"
               << dstRoot->getName() << "'";
    } else {
      // FR-72: source and destination in the same PARAMETER reject. The
      // array shape below is a deliberate UB refinement over a PROVABLE
      // whole region; a parameter's extent is not provable, and silently
      // riding `copy_within` would make an overlapping (C-undefined)
      // memcpy uncertifiable by the byte-diff oracle.
      if (llvm::isa<clang::ParmVarDecl>(dstRoot))
        return emitError(loc)
               << "unsupported: " << name
               << " source and destination point into the same slice "
                  "parameter '"
               << dstRoot->getName() << "'";
      // Both arguments point into the same object: two slice borrows
      // would alias a mutable borrow, so the whole array is borrowed
      // mutably once and the helper receives both element cursors
      // (`copy_within`; its memmove semantics refine C's undefined
      // overlapping memcpy).
      PtrExprValue whole{
          dstRoot, createIntConstant(loc, builder.getIntegerType(64), 0),
          Value()};
      FailureOr<Value> slice =
          emitCharRegionSlice(loc, whole, /*isMut=*/true);
      if (failed(slice))
        return failure();
      requestStringHelper("__emitrust_memcpy_within");
      builder.create<emitrust::CallOpaqueOp>(
          loc, TypeRange(),
          builder.getStringAttr("__emitrust_memcpy_within"),
          /*args=*/ArrayAttr(),
          ValueRange{*slice, dst->pointer.cursor, src->pointer.cursor,
                     count});
      return success();
    }
  }
  // FR-146: two regions of the SAME heap allocation share no named base
  // (both roots are null), so the (root, path) key above cannot see the
  // collision; their identity is the synthesized backing place. The
  // `copy_within` refinement above needs a PROVABLE whole region, which a
  // pointer's cursor into an allocation is not, so the pair rejects
  // rather than emitting the two-borrow form (rustc E0502).
  if (!dst->isMember() && !src->isMember() && dst->pointer.backing &&
      dst->pointer.backing == src->pointer.backing)
    return emitError(loc) << "unsupported: " << name
                          << " source and destination point into the same "
                             "allocation";
  FailureOr<Value> dstSlice = emitByteRegionSlice(loc, *dst, /*isMut=*/true,
                                                  /*allowUnsignedByte=*/true);
  if (failed(dstSlice))
    return failure();
  FailureOr<Value> srcSlice = emitByteRegionSlice(
      loc, *src, /*isMut=*/false, /*allowUnsignedByte=*/true);
  if (failed(srcSlice))
    return failure();
  // FR-72 element agreement: no helper signature fits an i8/ui8 mix (a
  // string-literal backing is i8; a uint8_t parameter is ui8), so a
  // mixed call rejects here instead of E0308 in the emitted crate.
  if (charRegionElement(*dstSlice) != charRegionElement(*srcSlice))
    return emitError(loc)
           << "unsupported: " << name
           << " arguments mix char and unsigned char regions";
  llvm::StringRef helper = isUnsignedByteRegion(*dstSlice)
                               ? "__emitrust_memcpy_u8"
                               : "__emitrust_memcpy";
  requestStringHelper(helper);
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr(helper),
      /*args=*/ArrayAttr(), ValueRange{*dstSlice, *srcSlice, count});
  return success();
}

LogicalResult CImporter::emitMultiBaseMemcpy(const clang::CallExpr *call,
                                             llvm::StringRef name,
                                             const CharRegionArg &dst,
                                             const clang::VarDecl *srcVar,
                                             Location loc) {
  Type ui8Type =
      IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  // Only the u8 member/window destination shape is admitted this wave;
  // everything else keeps the located multi-base rejection the pointer
  // channel would have raised.
  if (!dst.isMember() || dst.memberElement != ui8Type)
    return emitError(loc) << "unsupported: passing a pointer bound to "
                             "multiple objects to a string function";
  FailureOr<Value> n = emitRValue(call->getArg(2));
  if (failed(n))
    return failure();
  if (!llvm::isa<IntegerType>((*n).getType()))
    return emitError(loc) << "unsupported: " << name << " count type";
  Value count = castToIntType(loc, *n, builder.getIntegerType(64));
  const PointerLocalInfo &info = pointerLocals.find(srcVar)->second;
  Value discriminant = loadPlace(loc, info.baseIndexCell);
  Value srcCursor =
      info.cursorCell
          ? loadPlace(loc, info.cursorCell)
          : createIntConstant(loc, builder.getIntegerType(64), 0);
  auto sliceUi8 = emitrust::SliceType::get(ui8Type);
  return emitMultiBaseDispatch(
      loc, info.multiBases, discriminant,
      [&](const PointerBaseKey &armBase) -> LogicalResult {
        if (armBase.member)
          return emitError(loc)
                 << "unsupported: passing a pointer bound to multiple "
                    "objects to a string function";
        if (armBase.var == dst.memberRoot) {
          // Same-root arm. Only the WINDOW destination (empty path:
          // absolute byte cursors over ONE region place) has a provable
          // relation to the source cursor; it rides the whole-region
          // copy_within image — memmove semantics, refining C's
          // undefined overlapping (and exact-overlap self-) memcpy.
          if (!dst.memberPath.empty())
            return emitError(loc)
                   << "unsupported: " << name
                   << " source and destination point into the same "
                      "object '"
                   << dst.memberRoot->getName() << "'";
          Value zero =
              createIntConstant(loc, builder.getIntegerType(64), 0);
          Value slice = builder
                            .create<emitrust::SliceOfOp>(
                                loc, emitrust::MutRefType::get(sliceUi8),
                                dst.memberPlace, zero,
                                /*is_mut=*/true)
                            .getResult();
          requestStringHelper("__emitrust_memcpy_within_u8");
          builder.create<emitrust::CallOpaqueOp>(
              loc, TypeRange(),
              builder.getStringAttr("__emitrust_memcpy_within_u8"),
              /*args=*/ArrayAttr(),
              ValueRange{slice, dst.memberCursor, srcCursor, count});
          return success();
        }
        // Cross-root arm: two disjoint regions, two slices.
        auto it = symbols.find(armBase.var);
        if (it == symbols.end())
          return emitError(loc) << "unsupported: pointer target '"
                                << armBase.var->getName()
                                << "' is not an importable place";
        auto lvalueType =
            llvm::dyn_cast<emitrust::LValueType>(it->second.getType());
        Type elementType;
        if (lvalueType) {
          if (auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(
                  lvalueType.getValueType()))
            elementType = arrayType.getElementType();
          else if (auto sliceType = llvm::dyn_cast<emitrust::SliceType>(
                       lvalueType.getValueType()))
            elementType = sliceType.getElementType();
        }
        if (elementType != ui8Type)
          return emitError(loc)
                 << "unsupported: " << name
                 << " arguments mix char and unsigned char regions";
        Value dstSlice = builder
                             .create<emitrust::SliceOfOp>(
                                 loc, emitrust::MutRefType::get(sliceUi8),
                                 dst.memberPlace, dst.memberCursor,
                                 /*is_mut=*/true)
                             .getResult();
        Value srcSlice = builder
                             .create<emitrust::SliceOfOp>(
                                 loc, emitrust::RefType::get(sliceUi8),
                                 it->second, srcCursor,
                                 /*is_mut=*/false)
                             .getResult();
        requestStringHelper("__emitrust_memcpy_u8");
        builder.create<emitrust::CallOpaqueOp>(
            loc, TypeRange(),
            builder.getStringAttr("__emitrust_memcpy_u8"),
            /*args=*/ArrayAttr(), ValueRange{dstSlice, srcSlice, count});
        return success();
      });
}

FailureOr<Value>
CImporter::emitStringCompareCall(const clang::CallExpr *call,
                                 llvm::StringRef name, bool hasCount) {
  Location loc = translateLoc(call->getBeginLoc());
  unsigned expected = hasCount ? 3 : 2;
  if (call->getNumArgs() != expected)
    return emitError(loc) << "unsupported: " << name << " requires exactly "
                          << expected << " arguments";
  // Both borrows are shared, so even two arguments into the same object
  // (FR-87: or the same member FIELD) coexist. Only the byte-family
  // memcmp carries a u8 helper image (FR-72) — and, FR-87, member-array
  // regions — strcmp/strncmp are NUL-terminated i8 string functions and
  // keep the i8-only region rule and the historical member frontier.
  bool isByteFamily = name == "memcmp";
  FailureOr<CharRegionArg> lhs = emitByteRegionArg(
      call->getArg(0), /*isMut=*/false, /*interceptMember=*/isByteFamily);
  if (failed(lhs))
    return failure();
  FailureOr<CharRegionArg> rhs = emitByteRegionArg(
      call->getArg(1), /*isMut=*/false, /*interceptMember=*/isByteFamily);
  if (failed(rhs))
    return failure();
  Value count;
  if (hasCount) {
    FailureOr<Value> n = emitRValue(call->getArg(2));
    if (failed(n))
      return failure();
    if (!llvm::isa<IntegerType>((*n).getType()))
      return emitError(loc) << "unsupported: " << name << " count type";
    count = castToIntType(loc, *n, builder.getIntegerType(64));
  }
  FailureOr<Value> lhsSlice = emitByteRegionSlice(
      loc, *lhs, /*isMut=*/false, /*allowUnsignedByte=*/isByteFamily);
  if (failed(lhsSlice))
    return failure();
  FailureOr<Value> rhsSlice = emitByteRegionSlice(
      loc, *rhs, /*isMut=*/false, /*allowUnsignedByte=*/isByteFamily);
  if (failed(rhsSlice))
    return failure();
  // FR-72 element agreement: an i8/ui8 mix has no helper signature that
  // fits both regions, so it rejects here instead of E0308 at rustc.
  if (charRegionElement(*lhsSlice) != charRegionElement(*rhsSlice))
    return emitError(loc)
           << "unsupported: " << name
           << " arguments mix char and unsigned char regions";
  std::string helper = ("__emitrust_" + name).str();
  if (isByteFamily && isUnsignedByteRegion(*lhsSlice))
    helper += "_u8";
  SmallVector<Value> operands{*lhsSlice, *rhsSlice};
  if (count)
    operands.push_back(count);
  requestStringHelper(helper);
  Value result = builder
                     .create<emitrust::CallOpaqueOp>(
                         loc, TypeRange{builder.getI32Type()},
                         builder.getStringAttr(helper),
                         /*args=*/ArrayAttr(), operands)
                     .getResult(0);
  // The declared result type is C's int (i32) for the standard
  // prototypes; convert defensively for K&R-style declarations.
  FailureOr<Type> resultType = mapType(call->getType(), loc);
  if (failed(resultType))
    return failure();
  auto intType = llvm::dyn_cast<IntegerType>(*resultType);
  if (!intType)
    return emitError(loc) << "unsupported: " << name << " result type";
  return castToIntType(loc, result, intType);
}

const clang::CallExpr *
CImporter::asHostedStrchrCall(const clang::Expr *expr, bool &reverse) const {
  const auto *call =
      llvm::dyn_cast<clang::CallExpr>(expr->IgnoreParenImpCasts());
  if (!call)
    return nullptr;
  const clang::FunctionDecl *callee = call->getDirectCallee();
  if (!callee || !callee->getDeclName().isIdentifier() ||
      callee->getDefinition())
    return nullptr;
  if (callee->getName() == "strchr") {
    reverse = false;
    return call;
  }
  if (callee->getName() == "strrchr") {
    reverse = true;
    return call;
  }
  return nullptr;
}

FailureOr<Value> CImporter::emitStrchrIndex(const clang::CallExpr *call,
                                            bool reverse,
                                            PtrExprValue &region) {
  Location loc = translateLoc(call->getBeginLoc());
  llvm::StringRef name = reverse ? "strrchr" : "strchr";
  if (call->getNumArgs() != 2)
    return emitError(loc) << "unsupported: " << name
                          << " requires exactly 2 arguments";
  FailureOr<PtrExprValue> pointer = emitCharRegionArg(call->getArg(0));
  if (failed(pointer))
    return failure();
  region = *pointer;
  FailureOr<Value> needle = emitRValue(call->getArg(1));
  if (failed(needle))
    return failure();
  if (!llvm::isa<IntegerType>((*needle).getType()))
    return emitError(loc) << "unsupported: " << name << " character type";
  Value byte = castToIntType(loc, *needle, builder.getI32Type());
  FailureOr<Value> slice =
      emitCharRegionSlice(loc, region, /*isMut=*/false);
  if (failed(slice))
    return failure();
  llvm::StringRef helper =
      reverse ? "__emitrust_strrchr" : "__emitrust_strchr";
  requestStringHelper(helper);
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{builder.getIntegerType(64)},
          builder.getStringAttr(helper),
          /*args=*/ArrayAttr(), ValueRange{*slice, byte})
      .getResult(0);
}

LogicalResult CImporter::emitPutchar(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: putchar requires exactly one argument";
  FailureOr<Value> value = emitRValue(call->getArg(0));
  if (failed(value))
    return failure();
  auto intType = llvm::dyn_cast<IntegerType>((*value).getType());
  if (!intType || intType.getWidth() == 1)
    return emitError(loc) << "unsupported: putchar argument must be an "
                             "integer";
  // C's putchar writes the argument converted to unsigned char; the
  // `__emitrust_fmt_c` helper performs that conversion (ASCII-only, see
  // design.md C99-48).
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("print!"),
      builder.getArrayAttr(
          {builder.getStringAttr("{}"), builder.getIndexAttr(0)}),
      ValueRange{wrapCharFormat(loc, *value)});
  return success();
}

std::optional<llvm::StringRef>
CImporter::hostedMathCallee(llvm::StringRef name) {
  // fabs/sqrt/floor/ceil are IEEE-754-exact (fabs/floor/ceil are exact
  // operations, sqrt is correctly rounded), so every conforming
  // implementation — glibc, Rust's f64 methods, LLVM's constant folder —
  // agrees bit for bit. sin carries no such mandate; it is accepted on
  // the weaker "both sides resolve to the platform libm" argument,
  // pinned differentially (design.md C99-48). exp/log/pow stay rejected
  // (see emitCall) rather than widening that exception.
  return llvm::StringSwitch<std::optional<llvm::StringRef>>(name)
      .Case("sin", "f64::sin")
      .Case("fabs", "f64::abs")
      .Case("sqrt", "f64::sqrt")
      .Case("floor", "f64::floor")
      .Case("ceil", "f64::ceil")
      .Default(std::nullopt);
}

FailureOr<Value> CImporter::emitHostedMathCall(const clang::CallExpr *call,
                                               llvm::StringRef rustCallee) {
  Location loc = translateLoc(call->getBeginLoc());
  FailureOr<Value> argument = emitRValue(call->getArg(0));
  if (failed(argument))
    return failure();
  // The callee's prototype is `double f(double)` (checked at the call
  // site), so clang has already converted the argument to double; anything
  // else indicates an importer bug rather than an unsupported program.
  if (!llvm::isa<Float64Type>((*argument).getType()))
    return emitError(loc) << "unsupported: " << rustCallee
                          << " argument is not a double";
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{builder.getF64Type()},
          builder.getStringAttr(rustCallee),
          /*args=*/ArrayAttr(), ValueRange{*argument})
      .getResult(0);
}

FailureOr<Value> CImporter::emitAbsCall(const clang::CallExpr *call,
                                        bool isLong) {
  Location loc = translateLoc(call->getBeginLoc());
  llvm::StringRef name = isLong ? "labs" : "abs";
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: " << name << " requires exactly one argument";
  FailureOr<Value> value = emitRValue(call->getArg(0));
  if (failed(value))
    return failure();
  if (!llvm::isa<IntegerType>((*value).getType()))
    return emitError(loc)
           << "unsupported: " << name << " argument must be an integer";
  // The prototype has already converted the argument to int/long;
  // normalize the width anyway (a K&R-style declaration may differ).
  IntegerType intType = builder.getIntegerType(isLong ? 64 : 32);
  Value argument = castToIntType(loc, *value, intType);
  // wrapping_abs: abs(INT_MIN)/labs(LONG_MIN) is C UB (7.20.6.1p2),
  // refined to the deterministic two's-complement wrap the differential
  // oracle's platform also produces; Rust's plain `abs` would panic only
  // in debug builds and is rejected as non-deterministic across profiles.
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{intType},
          builder.getStringAttr(isLong ? "i64::wrapping_abs"
                                       : "i32::wrapping_abs"),
          /*args=*/ArrayAttr(), ValueRange{argument})
      .getResult(0);
}

FailureOr<Value> CImporter::emitAtoiCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: atoi requires exactly one argument";
  FailureOr<PtrExprValue> pointer = emitCharRegionArg(call->getArg(0));
  if (failed(pointer))
    return failure();
  FailureOr<Value> slice =
      emitCharRegionSlice(loc, *pointer, /*isMut=*/false);
  if (failed(slice))
    return failure();
  requestStringHelper("__emitrust_atoi");
  Value parsed =
      builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{builder.getI32Type()},
              builder.getStringAttr("__emitrust_atoi"),
              /*args=*/ArrayAttr(), ValueRange{*slice})
          .getResult(0);
  // atoi returns int; convert to the call's declared result type in case
  // a K&R-style declaration says otherwise (matching emitStrlenCall).
  FailureOr<Type> resultType = mapType(call->getType(), loc);
  if (failed(resultType))
    return failure();
  auto intType = llvm::dyn_cast<IntegerType>(*resultType);
  if (!intType)
    return emitError(loc) << "unsupported: atoi result type";
  return castToIntType(loc, parsed, intType);
}

LogicalResult CImporter::emitExitCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: exit requires exactly one argument";
  FailureOr<Value> status = emitRValue(call->getArg(0));
  if (failed(status))
    return failure();
  if (!llvm::isa<IntegerType>((*status).getType()))
    return emitError(loc)
           << "unsupported: exit status must be an integer";
  Value code = castToIntType(loc, *status, builder.getI32Type());
  // `std::process::exit` terminates with the given status like C's exit;
  // both report the low byte to the OS on this target. Its `!` result
  // needs no representation — the call is statement-position only.
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("std::process::exit"),
      /*args=*/ArrayAttr(), ValueRange{code});
  return success();
}

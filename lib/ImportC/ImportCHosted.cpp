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
/// emitFileClose/emitFileTruth), the FR-183 standard-input family
/// (emitStdinHandle/emitScanfCall/emitScanArg/emitGetchar), the string.h
/// family
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
    requestFileHelper("__emitrust_fgetc");
  // FR-183: the fgetc primitive's `Stdin` arm reads standard input through
  // the shared single-pushback byte reader, so requesting fgetc pulls that
  // reader (and its slot) in too -- otherwise the emitted enum arm would
  // name a function the crate never defines.
  if (name == "__emitrust_fgetc")
    requestScanHelper("__emitrust_stdin_getc");
  neededFileHelpers.insert(name);
}

void CImporter::requestScanHelper(llvm::StringRef name) {
  // Every standard-input helper reads through the single-pushback byte
  // primitive, so the slot and the reader come along with any of them.
  neededScanHelpers.insert("__EMITRUST_STDIN_PUSHBACK");
  neededScanHelpers.insert("__emitrust_stdin_getc");
  // The directives that skip leading whitespace also push a character back
  // (C affords exactly one slot; see `__emitrust_scan_d`).
  if (name == "__emitrust_scan_ws" || name == "__emitrust_scan_d" ||
      name == "__emitrust_scan_u" || name == "__emitrust_scan_f" ||
      name == "__emitrust_scan_lf") {
    neededScanHelpers.insert("__emitrust_stdin_ungetc");
    neededScanHelpers.insert("__emitrust_scan_skip_ws");
  }
  // `%f` and `%lf` are the same greedy scan of C's float grammar over two
  // out-argument types, so both pull the one shared scanner (and its
  // `inf`/`nan` keyword arm).
  if (name == "__emitrust_scan_f" || name == "__emitrust_scan_lf") {
    neededScanHelpers.insert("__emitrust_scan_float_text");
    neededScanHelpers.insert("__emitrust_scan_float_word");
  }
  neededScanHelpers.insert(name);
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
                                              bool isMut, bool allowStdin) {
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
  if (!place) {
    // FR-183: the standard streams are not owned handles. `stdin` is the
    // STATELESS `Stdin` unit variant and materializes a fresh temporary at
    // each use site, which is exactly what dissolves the ownership
    // question a process-global FILE* would raise. Every other standard
    // stream use is refused, and one of them would MISCOMPILE if admitted:
    // `fclose(stdin)` writes Null through a per-use temporary and is a
    // silent no-op, so a program that read past its own fclose would get
    // bytes instead of undefined behavior.
    if (var && var->getDeclName().isIdentifier() &&
        isFilePtrType(var->getType())) {
      llvm::StringRef stream = canonicalStreamName(var->getName());
      if (stream == "stdin" || stream == "stdout" || stream == "stderr") {
        if (stream == "stdin" && allowStdin)
          return emitStdinHandle(loc, isMut);
        return emitError(loc)
               << "unsupported: '" << stream
               << "' as a FILE* stream here (only sequential reads of "
                  "'stdin' are supported)";
      }
    }
    return emitError(loc) << "unsupported: a FILE* stream argument must be "
                             "a function-local FILE* variable";
  }
  Type refType = isMut ? Type(emitrust::MutRefType::get(fileHandleType()))
                       : Type(emitrust::RefType::get(fileHandleType()));
  return builder.create<emitrust::AddrOfOp>(loc, refType, place, isMut)
      .getResult();
}

FailureOr<Value> CImporter::emitStdinHandle(Location loc, bool isMut) {
  requestFileHelper("__emitrust_stdin");
  // A fresh place per use site: the variant carries no state, so two
  // temporaries naming standard input are indistinguishable, and the
  // borrow below is the only reference that ever exists to either.
  Value place = createVariablePlace(loc, fileHandleType(), std::string());
  Value handle =
      builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{fileHandleType()},
              builder.getStringAttr("__emitrust_stdin"), /*args=*/ArrayAttr(),
              ValueRange{})
          .getResult(0);
  builder.create<emitrust::AssignOp>(loc, place, handle);
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
  FailureOr<Value> handle = emitFileHandleArg(call->getArg(0), /*isMut=*/true,
                                              /*allowStdin=*/true);
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
  // FR-183: only the READ direction admits `stdin`; `fwrite` to it (and
  // any reader on `stdout`/`stderr`) stays a located rejection.
  FailureOr<Value> handle = emitFileHandleArg(call->getArg(3), /*isMut=*/true,
                                              /*allowStdin=*/!isWrite);
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
  FailureOr<Value> handle = emitFileHandleArg(call->getArg(2), /*isMut=*/true,
                                              /*allowStdin=*/true);
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

//===----------------------------------------------------------------------===//
// FR-183: standard input (scanf / fscanf / getchar over `stdin`)
//===----------------------------------------------------------------------===//

/// C's whitespace set in the "C" locale: space plus the five control
/// characters 9..=13. A run of these in a scanf format is one directive.
static bool isScanSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

FailureOr<Value> CImporter::emitScanArg(const clang::Expr *expr, ScanKind kind,
                                        llvm::StringRef &helper) {
  const clang::Expr *e = expr->IgnoreParenImpCasts();
  Location loc = translateLoc(e->getBeginLoc());
  // Exactly `&L` for a function-local scalar. An explicit cast is NOT
  // stripped here on purpose (`IgnoreParenImpCasts` leaves a
  // CStyleCastExpr standing), and `&arr[i]` / `&s.f` / `&global` / a bare
  // pointer variable all fall out of the DeclRefExpr test below: an
  // element or member address routes through the FR-40 owner rewrite,
  // which cannot apply to a `call_opaque` helper argument.
  const auto *addrOf = llvm::dyn_cast<clang::UnaryOperator>(e);
  const clang::DeclRefExpr *ref =
      addrOf && addrOf->getOpcode() == clang::UO_AddrOf
          ? llvm::dyn_cast<clang::DeclRefExpr>(
                addrOf->getSubExpr()->IgnoreParenImpCasts())
          : nullptr;
  const auto *var =
      ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
  if (!var || !var->hasLocalStorage() || llvm::isa<clang::ParmVarDecl>(var) ||
      !var->getType()->isScalarType())
    return emitError(loc) << "unsupported: a scanf argument must be the "
                             "address of a function-local scalar variable";
  // The C type must be EXACTLY the conversion's own; scanf is variadic, so
  // no implicit conversion ever fixes a mismatch and a wrong width would
  // silently write the wrong bytes in C too.
  clang::QualType argType = var->getType();
  switch (kind) {
  case ScanKind::Int:
    if (!astContext().hasSameUnqualifiedType(argType, astContext().IntTy))
      return emitError(loc)
             << "unsupported: scanf conversion '%d' requires an 'int' "
                "argument";
    break;
  case ScanKind::Unsigned:
    if (!astContext().hasSameUnqualifiedType(argType,
                                             astContext().UnsignedIntTy))
      return emitError(loc)
             << "unsupported: scanf conversion '%u' requires an 'unsigned "
                "int' argument";
    break;
  case ScanKind::Char:
    if (!astContext().hasSameUnqualifiedType(argType, astContext().CharTy) &&
        !astContext().hasSameUnqualifiedType(argType,
                                             astContext().SignedCharTy) &&
        !astContext().hasSameUnqualifiedType(argType,
                                             astContext().UnsignedCharTy))
      return emitError(loc) << "unsupported: scanf conversion '%c' requires "
                               "a character argument";
    break;
  case ScanKind::Float:
    if (!astContext().hasSameUnqualifiedType(argType, astContext().FloatTy))
      return emitError(loc)
             << "unsupported: scanf conversion '%f' requires a 'float' "
                "argument";
    break;
  case ScanKind::Double:
    if (!astContext().hasSameUnqualifiedType(argType, astContext().DoubleTy))
      return emitError(loc)
             << "unsupported: scanf conversion '%lf' requires a 'double' "
                "argument";
    break;
  case ScanKind::Whitespace:
    llvm_unreachable("a whitespace directive consumes no argument");
  }
  FailureOr<Type> valueType = mapType(argType, loc);
  if (failed(valueType))
    return failure();
  auto intType = llvm::dyn_cast<IntegerType>(*valueType);
  if (!intType && !llvm::isa<FloatType>(*valueType))
    return emitError(loc) << "unsupported: a scanf argument must be the "
                             "address of a function-local scalar variable";
  switch (kind) {
  case ScanKind::Int:
    helper = "__emitrust_scan_d";
    break;
  case ScanKind::Unsigned:
    helper = "__emitrust_scan_u";
    break;
  case ScanKind::Char:
    // `char` is signed on some targets and unsigned on others; the helper
    // pair keeps the out-reference's type EXACT either way rather than
    // making `%c` a platform-dependent rejection.
    helper = intType.isUnsigned() ? "__emitrust_scan_c_u"
                                  : "__emitrust_scan_c";
    break;
  case ScanKind::Float:
    helper = "__emitrust_scan_f";
    break;
  case ScanKind::Double:
    helper = "__emitrust_scan_lf";
    break;
  case ScanKind::Whitespace:
    break;
  }
  FailureOr<Value> place = emitLValue(addrOf->getSubExpr());
  if (failed(place))
    return failure();
  auto lvalueType = llvm::dyn_cast<emitrust::LValueType>((*place).getType());
  if (!lvalueType || lvalueType.getValueType() != *valueType)
    return emitError(loc) << "unsupported: a scanf argument must be the "
                             "address of a function-local scalar variable";
  return builder
      .create<emitrust::AddrOfOp>(loc, emitrust::MutRefType::get(*valueType),
                                  *place, /*is_mut=*/true)
      .getResult();
}

FailureOr<Value> CImporter::emitScanfCall(const clang::CallExpr *call,
                                          unsigned formatIndex) {
  Location loc = translateLoc(call->getBeginLoc());
  llvm::StringRef name = formatIndex == 0 ? "scanf" : "fscanf";
  if (formatIndex > 0) {
    if (call->getNumArgs() < 1)
      return emitError(loc)
             << "unsupported: fscanf without a stream argument";
    // Only the literal `stdin` (canonicalized through Darwin's `__stdinp`
    // spelling). A real FILE* stream would need the whole scanner over an
    // owned handle, which no measured corpus case asks for.
    const auto *stream = llvm::dyn_cast<clang::DeclRefExpr>(
        call->getArg(0)->IgnoreParenImpCasts());
    const clang::NamedDecl *streamDecl =
        stream ? llvm::dyn_cast<clang::NamedDecl>(stream->getDecl()) : nullptr;
    if (!streamDecl || !streamDecl->getDeclName().isIdentifier() ||
        canonicalStreamName(streamDecl->getName()) != "stdin")
      return emitError(translateLoc(call->getArg(0)->getBeginLoc()))
             << "unsupported: fscanf on a FILE* stream other than stdin";
  }
  if (call->getNumArgs() <= formatIndex)
    return emitError(loc) << "unsupported: " << name
                          << " without a format string";
  const auto *literal = llvm::dyn_cast<clang::StringLiteral>(
      call->getArg(formatIndex)->IgnoreParenImpCasts());
  if (!literal || !literal->isOrdinary())
    return emitError(loc)
           << "unsupported: scanf format must be an ordinary string literal";
  llvm::StringRef format = literal->getString();
  for (char c : format)
    if (!isScanSpace(c) && (c < 0x20 || c > 0x7e))
      return emitError(loc) << "unsupported: non-printable or non-ASCII byte "
                               "in a scanf format";

  // The admitted grammar: whitespace runs, `%d`, `%u`, `%c` and C's float
  // family `a e f g` in either case, optionally preceded by `l`. No other
  // length modifier, no field width, no assignment-suppressing `*`, no
  // literal-match characters.
  //
  // The float family is ONE conversion with fourteen spellings -- C reads
  // the same input syntax for every one of them, and the `l` selects only
  // the out-argument's type. FR-183 refused it because glibc's grammar
  // accepts hex floats (`0x1p3` is 8) and `nan(1)`, which Rust's `parse`
  // does not; the decimal / `inf` / `nan` subset IS bit-exact (both
  // `strtof` and Rust's parser are correctly rounded), and the two forms
  // Rust cannot reproduce arrive from RUNTIME stdin, so they cannot be
  // refused HERE at all -- `__emitrust_scan_float_text` panics loudly on
  // them instead. `%L` stays refused: long double is x87 80-bit.
  SmallVector<ScanKind> directives;
  for (size_t i = 0; i < format.size();) {
    if (isScanSpace(format[i])) {
      while (i < format.size() && isScanSpace(format[i]))
        ++i;
      directives.push_back(ScanKind::Whitespace);
      continue;
    }
    if (format[i] != '%')
      return emitError(loc) << "unsupported: literal-match character '"
                            << format[i] << "' in a scanf format";
    size_t start = i++;
    if (i >= format.size())
      return emitError(loc)
             << "unsupported: trailing '%' in a scanf format";
    // The directive's spelling for the diagnostic: '%' through the first
    // character that is not a flag, width digit or length modifier.
    size_t end = i;
    while (end < format.size() &&
           (llvm::isDigit(format[end]) ||
            llvm::StringRef("*hljztL").contains(format[end])))
      ++end;
    if (end < format.size())
      ++end;
    llvm::StringRef spelling = format.substr(start, end - start);
    if (format[i] == '*')
      return emitError(loc) << "unsupported: scanf assignment suppression in '"
                            << spelling << "'";
    if (llvm::isDigit(format[i]))
      return emitError(loc)
             << "unsupported: scanf field width in '" << spelling << "'";
    // `l` is admitted ONLY as the float family's `double` selector; before
    // anything else it is still a length-modifier rejection, and so is
    // every other modifier including `L`.
    llvm::StringRef floatConversions = "aAeEfFgG";
    bool isDouble = false;
    if (format[i] == 'l' && i + 1 < format.size() &&
        floatConversions.contains(format[i + 1])) {
      isDouble = true;
      ++i;
    } else if (llvm::StringRef("hljztL").contains(format[i])) {
      return emitError(loc)
             << "unsupported: scanf length modifier in '" << spelling << "'";
    }
    char conversion = format[i++];
    if (conversion == 'd')
      directives.push_back(ScanKind::Int);
    else if (conversion == 'u')
      directives.push_back(ScanKind::Unsigned);
    else if (conversion == 'c')
      directives.push_back(ScanKind::Char);
    else if (floatConversions.contains(conversion))
      directives.push_back(isDouble ? ScanKind::Double : ScanKind::Float);
    else
      return emitError(loc)
             << "unsupported: scanf conversion '%" << conversion
             << "' (only %d, %u, %c and %a/%e/%f/%g are supported)";
  }

  unsigned conversions = 0;
  for (ScanKind kind : directives)
    if (kind != ScanKind::Whitespace)
      ++conversions;
  if (conversions != call->getNumArgs() - formatIndex - 1)
    return emitError(loc) << "unsupported: scanf conversion count does not "
                             "match the argument count";

  // The chain. `s >= 0` is "running, s conversions assigned"; `s < 0` is
  // "stopped, the answer is -s - 2". Every helper no-ops on a stopped
  // state, so C's early exit needs no control flow here -- and each helper
  // takes at most one out-reference, so only ONE `&mut` is ever live.
  Type stateType = builder.getI32Type();
  Value state = createIntConstant(loc, stateType, 0);
  unsigned argCursor = formatIndex + 1;
  for (ScanKind kind : directives) {
    llvm::StringRef helper = "__emitrust_scan_ws";
    SmallVector<Value> operands{state};
    if (kind != ScanKind::Whitespace) {
      FailureOr<Value> outRef =
          emitScanArg(call->getArg(argCursor++), kind, helper);
      if (failed(outRef))
        return failure();
      operands.push_back(*outRef);
    }
    requestScanHelper(helper);
    state = builder
                .create<emitrust::CallOpaqueOp>(
                    loc, TypeRange{stateType}, builder.getStringAttr(helper),
                    /*args=*/ArrayAttr(), operands)
                .getResult(0);
  }
  requestScanHelper("__emitrust_scan_done");
  Value result = builder
                     .create<emitrust::CallOpaqueOp>(
                         loc, TypeRange{stateType},
                         builder.getStringAttr("__emitrust_scan_done"),
                         /*args=*/ArrayAttr(), ValueRange{state})
                     .getResult(0);
  // C's return type is `int`; convert only if a declaration says otherwise.
  FailureOr<Type> resultType = mapType(call->getType(), loc);
  if (succeeded(resultType))
    if (auto intType = llvm::dyn_cast<IntegerType>(*resultType);
        intType && intType != stateType)
      return castToIntType(loc, result, intType);
  return result;
}

FailureOr<Value> CImporter::emitGetchar(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 0)
    return emitError(loc) << "unsupported: getchar takes no arguments";
  requestScanHelper("__emitrust_stdin_getc");
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{builder.getI32Type()},
          builder.getStringAttr("__emitrust_stdin_getc"), /*args=*/ArrayAttr(),
          ValueRange{})
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
  // Two DISTINCT parameter declarations of one Phase-4 owner method are
  // still two cursors into ONE region (`planOwners` promotes over a
  // single storage base, so every data-pointer parameter is an i64 index
  // into the receiver's `self.data`), which the base check above keys on
  // declarations and cannot see. The byte-family copies have a
  // same-region image for exactly this — `__emitrust_memcpy_within`'s
  // `copy_within` — but the str*-family copies do NOT: their helpers
  // discover the length from the source's NUL while writing the
  // destination, and no single-slice image of that exists in the tree.
  // Emitting the two-slice form here is `&mut self.data[dst..]` next to
  // `&self.data[src..]`: rustc E0502, a crate that does not build, after
  // the tool exited 0. So the shape rejects LOUDLY and located instead.
  // The diagnostic names the OWNER ARRAY, not a parameter: the parameters
  // are cursors, the region is the owner's array. The COMPARISON members
  // of the family never reach here — two shared borrows are legal Rust.
  if (dst->base && src->base && isCurrentOwnerRegionRoot(dst->base) &&
      isCurrentOwnerRegionRoot(src->base))
    return emitError(loc) << "unsupported: " << name
                          << " source and destination point into the same "
                             "owner region '"
                          << currentMethodOwner->getName() << "'";
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
  // FR-229 Wave 2 (capability C): `memcpy(dst, &obj, sizeof obj)` — the
  // source is the OBJECT REPRESENTATION of a local scalar or padding-free
  // aggregate, which the typed model never materializes, so there is no
  // source region for `emitByteRegionArg` below to resolve at all (it
  // rejects with `the address of a scalar object is not a string region`,
  // which is where the corpus 037/038/039 family dies today). The
  // representation is scattered directly into `dst` instead. Every shape
  // this declines falls straight through to that unchanged rejection.
  {
    FailureOr<bool> handled =
        tryEmitObjectRepresentationMemcpy(call, name, *dst, loc);
    if (failed(handled))
      return failure();
    if (*handled)
      return success();
  }
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
  // Two DISTINCT parameter declarations of one Phase-4 owner method are
  // still two cursors into ONE region: `planOwners` promotes a class
  // all-or-nothing over a single storage base
  // (`info.storageBases.size() != 1` disqualifies otherwise), so every
  // data-pointer parameter of the method is an i64 element index into the
  // receiver's `self.data` and `symbols` maps them all to the SAME
  // `receiverDataPlace`. The (root, field-path) key above keys on the
  // DECLARATION, so it cannot see that, and the pair fell through to the
  // two-slice image below: `&mut self.data[dst..]` alongside
  // `&self.data[src..]`, rustc E0502, a crate that does not build, with
  // the tool still exiting 0 (the FR-140/141/142/146 silent-unbuildable
  // class). The FR-72 objection that made a same-PARAMETER copy reject —
  // a parameter's extent is not provable — does not apply here: the
  // extent IS provable, it is the owner's own array, and the cursors are
  // ABSOLUTE offsets into it, which is exactly the condition the same-root
  // array branch above rides `copy_within` on. So this joins that image
  // (memmove semantics, refining C's undefined OVERLAPPING memcpy exactly
  // as the same-root branch does; a count that runs off the region panics
  // where C read out of bounds — the loud direction, never a wrong byte).
  // Only the plain single-base pointer shape qualifies: a literal
  // backing, a heap backing, an unwrapped nullable slice, a `&s.member`
  // base or a multi-base cursor resolves through a place that is NOT the
  // receiver data member, so those decline to the historical images.
  if (!dst->isMember() && !src->isMember() && dstRoot && srcRoot &&
      isCurrentOwnerRegionRoot(dstRoot) && isCurrentOwnerRegionRoot(srcRoot) &&
      !dst->pointer.literalBacking && !src->pointer.literalBacking &&
      !dst->pointer.backing && !src->pointer.backing &&
      !dst->pointer.slicePlace && !src->pointer.slicePlace &&
      !dst->pointer.member && !src->pointer.member &&
      !dst->pointer.baseIndex && !src->pointer.baseIndex &&
      dst->pointer.multiBases.empty() && src->pointer.multiBases.empty()) {
    // Defensive: the shared region is only provable when both roots
    // really resolve to one place. Every owner-region parameter is bound
    // to `receiverDataPlace` in the method prologue, so this holds by
    // construction; a mismatch would be a planning bug, and rejecting is
    // safer than emitting a copy over the wrong region.
    auto dstIt = symbols.find(dstRoot);
    auto srcIt = symbols.find(srcRoot);
    if (dstIt == symbols.end() || srcIt == symbols.end() ||
        dstIt->second != srcIt->second)
      return emitError(loc) << "unsupported: " << name
                            << " source and destination point into the same "
                               "owner region '"
                            << currentMethodOwner->getName() << "'";
    // The WHOLE receiver region is borrowed (cursor 0): the helper's
    // dst/src cursors are absolute element offsets into it, so starting
    // the slice at the dst cursor would shift the copy window.
    PtrExprValue whole{dstRoot,
                       createIntConstant(loc, builder.getIntegerType(64), 0),
                       Value()};
    FailureOr<Value> slice = emitCharRegionSlice(loc, whole, /*isMut=*/true,
                                                 /*allowUnsignedByte=*/true);
    if (failed(slice))
      return failure();
    llvm::StringRef helper = isUnsignedByteRegion(*slice)
                                 ? "__emitrust_memcpy_within_u8"
                                 : "__emitrust_memcpy_within";
    requestStringHelper(helper);
    builder.create<emitrust::CallOpaqueOp>(
        loc, TypeRange(), builder.getStringAttr(helper),
        /*args=*/ArrayAttr(),
        ValueRange{*slice, dst->pointer.cursor, src->pointer.cursor, count});
    return success();
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

FailureOr<Value> CImporter::emitStrSpanCall(const clang::CallExpr *call,
                                            llvm::StringRef name) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 2)
    return emitError(loc) << "unsupported: " << name
                          << " requires exactly 2 arguments";
  // FR-224: strcspn/strspn are pure byte SCANS over two NUL-terminated
  // string regions -- exactly emitStringCompareCall's shape (two shared
  // borrows, so the two arguments may name the same object) with a
  // size_t result instead of an int. They keep the i8-only region rule
  // that strcmp/strncmp keep: these are <string.h> string functions, not
  // the sign-agnostic byte family, so there is no u8 helper image and an
  // unsigned char region rejects rather than silently taking one.
  FailureOr<CharRegionArg> lhs = emitByteRegionArg(
      call->getArg(0), /*isMut=*/false, /*interceptMember=*/false);
  if (failed(lhs))
    return failure();
  FailureOr<CharRegionArg> rhs = emitByteRegionArg(
      call->getArg(1), /*isMut=*/false, /*interceptMember=*/false);
  if (failed(rhs))
    return failure();
  FailureOr<Value> lhsSlice = emitByteRegionSlice(
      loc, *lhs, /*isMut=*/false, /*allowUnsignedByte=*/false);
  if (failed(lhsSlice))
    return failure();
  FailureOr<Value> rhsSlice = emitByteRegionSlice(
      loc, *rhs, /*isMut=*/false, /*allowUnsignedByte=*/false);
  if (failed(rhsSlice))
    return failure();
  std::string helper = ("__emitrust_" + name).str();
  requestStringHelper(helper);
  Value count = builder
                    .create<emitrust::CallOpaqueOp>(
                        loc, TypeRange{builder.getIntegerType(64)},
                        builder.getStringAttr(helper),
                        /*args=*/ArrayAttr(),
                        ValueRange{*lhsSlice, *rhsSlice})
                    .getResult(0);
  // Convert the i64 count to the call's declared result type (size_t for
  // the standard prototype), matching emitStrlenCall.
  FailureOr<Type> resultType = mapType(call->getType(), loc);
  if (failed(resultType))
    return failure();
  auto intType = llvm::dyn_cast<IntegerType>(*resultType);
  if (!intType)
    return emitError(loc) << "unsupported: " << name << " result type";
  return castToIntType(loc, count, intType);
}

FailureOr<Value> CImporter::emitAtofCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: atof requires exactly one argument";
  FailureOr<PtrExprValue> pointer = emitCharRegionArg(call->getArg(0));
  if (failed(pointer))
    return failure();
  FailureOr<Value> slice =
      emitCharRegionSlice(loc, *pointer, /*isMut=*/false);
  if (failed(slice))
    return failure();
  // FR-234 rung 2: `__emitrust_atof` is a PROJECTION of
  // `__emitrust_atof_end`, so the pair is requested together -- the helper
  // table has no dependency edges, and an emitted projection without its
  // scan would not compile (the loud direction).
  requestStringHelper("__emitrust_atof");
  requestStringHelper("__emitrust_atof_end");
  Value parsed =
      builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{builder.getF64Type()},
              builder.getStringAttr("__emitrust_atof"),
              /*args=*/ArrayAttr(), ValueRange{*slice})
          .getResult(0);
  // atof returns double; the standard prototype's result type IS double,
  // but convert defensively for a K&R-style declaration (matching
  // emitAtoiCall / emitStrlenCall).
  FailureOr<Type> resultType = mapType(call->getType(), loc);
  if (failed(resultType))
    return failure();
  if (*resultType == parsed.getType())
    return parsed;
  if (llvm::isa<Float32Type>(*resultType))
    return builder.create<arith::TruncFOp>(loc, *resultType, parsed)
        .getResult();
  return emitError(loc) << "unsupported: atof result type";
}

// FR-234 rung 2: the shared `endptr` classifier for the strto* family.
// NULL is rung 1's admission; `&end` over a decomposed pointer local is
// rung 2's; every other spelling keeps the rung-1 rejection, whose wording
// carries the `strtox-endptr` ledger needle.
FailureOr<const clang::VarDecl *>
CImporter::classifyStrtoEndptr(const clang::CallExpr *call,
                               llvm::StringRef name, unsigned index) {
  const clang::Expr *endptr = call->getArg(index);
  Location loc = translateLoc(endptr->getBeginLoc());
  if (isNullPointerConstantExpr(endptr))
    return static_cast<const clang::VarDecl *>(nullptr);
  // Mirrors `PointerRegionAnalysis`'s admission exactly (ImportC.cpp): the
  // SAME peel, the SAME `&local` test. If the two disagreed the region
  // would have been joined without a write, or written without a join --
  // so this re-derives the shape rather than trusting a flag.
  const clang::Expr *argument = stripTrivia(endptr);
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(argument))
    argument = stripTrivia(cast->getSubExpr());
  if (const auto *addrOf = llvm::dyn_cast<clang::UnaryOperator>(argument);
      addrOf && addrOf->getOpcode() == clang::UO_AddrOf)
    if (const clang::VarDecl *pointer = asLocalVarRef(addrOf->getSubExpr()))
      if (pointerLocals.contains(pointer))
        return pointer;
  // The location is the ARGUMENT's, not the call's: at a real site the
  // interesting token is the `&end`.
  return emitError(loc) << "unsupported: " << name
                        << " with a non-null endptr argument";
}

LogicalResult CImporter::emitStrtoEndptrWrite(Location loc,
                                              llvm::StringRef name,
                                              const clang::VarDecl *endptr,
                                              const PtrExprValue &region,
                                              Value offset) {
  auto it = pointerLocals.find(endptr);
  if (it == pointerLocals.end()) // Defensive; the classifier required it.
    return emitError(loc) << "unsupported: " << name
                          << " endptr must walk the parsed string's region";
  const PointerLocalInfo &info = it->second;
  // The cursor C hands back is an offset into ARGUMENT 0's region. A
  // pointer that walks anything else -- a different object, a multi-base
  // region (CTS-P7), a member-rooted or heap-backed one that argument 0
  // does not share -- has nowhere to put that answer, so it is refused
  // rather than approximated. `region.cursor` is required for the same
  // reason: a degenerate base has no element offset to advance.
  if (!info.multiBases.empty() || region.baseIndex || !info.cursorCell ||
      !region.cursor || info.base != region.base ||
      info.member != region.member ||
      info.literalBacking != region.literalBacking ||
      info.backing != region.backing)
    return emitError(loc) << "unsupported: " << name
                          << " endptr must walk the parsed string's region";
  Value absolute =
      builder.create<arith::AddIOp>(loc, region.cursor, offset).getResult();
  // C 7.22.1.4p7 stores a pointer INTO the subject string -- never a null
  // one, and never nothing, including the no-conversion case (where the
  // stored pointer is `nptr` itself, which is exactly `region.cursor + 0`).
  // So a nullable `endptr` takes a definite `true`, not the co-argument's
  // own flag.
  if (info.nonNullCell)
    builder.create<memref::StoreOp>(loc, createBoolConstant(loc, true),
                                    info.nonNullCell);
  builder.create<memref::StoreOp>(loc, absolute, info.cursorCell);
  return success();
}

FailureOr<Value> CImporter::emitStrtoIntCall(const clang::CallExpr *call,
                                             llvm::StringRef name,
                                             bool isUnsigned) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 3)
    return emitError(loc)
           << "unsupported: " << name << " requires exactly 3 arguments";
  // Classified BEFORE the region is emitted so the frontier reads off the
  // endptr rather than off whatever the region lowering happens to say
  // about `&end`.
  FailureOr<const clang::VarDecl *> endptr =
      classifyStrtoEndptr(call, name, /*index=*/1);
  if (failed(endptr))
    return failure();
  FailureOr<PtrExprValue> pointer = emitCharRegionArg(call->getArg(0));
  if (failed(pointer))
    return failure();
  FailureOr<Value> slice =
      emitCharRegionSlice(loc, *pointer, /*isMut=*/false);
  if (failed(slice))
    return failure();
  FailureOr<Value> base = emitRValue(call->getArg(2));
  if (failed(base))
    return failure();
  if (!llvm::isa<IntegerType>((*base).getType()))
    return emitError(loc) << "unsupported: " << name
                          << " base must be an integer";
  Value radix = castToIntType(loc, *base, builder.getI32Type());
  std::string helper = ("__emitrust_" + name).str();
  requestStringHelper(helper);
  // Both wrappers delegate to the shared scan, so it has to be requested
  // explicitly -- the helper table has no dependency edges and emits
  // exactly the names that were asked for.
  requestStringHelper("__emitrust_strto_scan");
  // The helper's result domain is the C function's own: saturation is
  // type-directed (LONG_MAX/LONG_MIN vs ULONG_MAX), so a shared i64
  // image with a cast on top would be a different function.
  Type helperResult =
      isUnsigned ? llvm::cast<Type>(IntegerType::get(builder.getContext(), 64,
                                                     IntegerType::Unsigned))
                 : llvm::cast<Type>(builder.getIntegerType(64));
  Value parsed = builder
                     .create<emitrust::CallOpaqueOp>(
                         loc, TypeRange{helperResult},
                         builder.getStringAttr(helper),
                         /*args=*/ArrayAttr(), ValueRange{*slice, radix})
                     .getResult(0);
  // FR-234 rung 2: C's second answer. ONE SCAN, TWO ENTRY POINTS -- the
  // cursor does not depend on the saturation limit (the subject sequence
  // is the whole run of digits however the accumulator overflows), so
  // `strtol` and `strtoul` of one string agree on it and rung 1's pinned
  // wrapper lowering above does not move. The SAME slice value feeds both
  // calls: the value and the cursor are two reads of one subject string.
  if (*endptr) {
    requestStringHelper("__emitrust_strto_end");
    Value cursor = builder
                       .create<emitrust::CallOpaqueOp>(
                           loc, TypeRange{builder.getIntegerType(64)},
                           builder.getStringAttr("__emitrust_strto_end"),
                           /*args=*/ArrayAttr(), ValueRange{*slice, radix})
                       .getResult(0);
    if (failed(emitStrtoEndptrWrite(loc, name, *endptr, *pointer, cursor)))
      return failure();
  }
  // Convert to the call's declared result type; the standard prototype
  // already says long/unsigned long, but a K&R-style declaration may
  // differ (matching emitAtoiCall / emitStrlenCall).
  FailureOr<Type> resultType = mapType(call->getType(), loc);
  if (failed(resultType))
    return failure();
  auto intType = llvm::dyn_cast<IntegerType>(*resultType);
  if (!intType)
    return emitError(loc) << "unsupported: " << name << " result type";
  return castToIntType(loc, parsed, intType);
}

FailureOr<Value> CImporter::emitStrtodCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 2)
    return emitError(loc)
           << "unsupported: strtod requires exactly 2 arguments";
  FailureOr<const clang::VarDecl *> endptr =
      classifyStrtoEndptr(call, "strtod", /*index=*/1);
  if (failed(endptr))
    return failure();
  // C 7.22.1.1p2: atof(s) IS strtod(s, NULL). Delegating rather than
  // duplicating is what keeps FR-235's classification (inf/nan parsed,
  // hex floats and NaN payloads a loud stop) from having a second,
  // divergent home. The panic text therefore still says `atof:` -- one
  // helper, one pinned wording.
  FailureOr<PtrExprValue> pointer = emitCharRegionArg(call->getArg(0));
  if (failed(pointer))
    return failure();
  FailureOr<Value> slice =
      emitCharRegionSlice(loc, *pointer, /*isMut=*/false);
  if (failed(slice))
    return failure();
  Value parsed;
  if (*endptr) {
    // FR-234 rung 2: the value and the cursor come out of ONE call, so
    // they cannot disagree -- `__emitrust_atof` is itself a projection of
    // this helper's first component, and the float grammar exists in
    // exactly one place (the FR-235 invariant).
    requestStringHelper("__emitrust_atof_end");
    auto both = builder.create<emitrust::CallOpaqueOp>(
        loc, TypeRange{builder.getF64Type(), builder.getIntegerType(64)},
        builder.getStringAttr("__emitrust_atof_end"),
        /*args=*/ArrayAttr(), ValueRange{*slice});
    parsed = both.getResult(0);
    if (failed(emitStrtoEndptrWrite(loc, "strtod", *endptr, *pointer,
                                    both.getResult(1))))
      return failure();
  } else {
    // The projection and its scan travel together (see `emitAtofCall`).
    requestStringHelper("__emitrust_atof");
    requestStringHelper("__emitrust_atof_end");
    parsed = builder
                 .create<emitrust::CallOpaqueOp>(
                     loc, TypeRange{builder.getF64Type()},
                     builder.getStringAttr("__emitrust_atof"),
                     /*args=*/ArrayAttr(), ValueRange{*slice})
                 .getResult(0);
  }
  FailureOr<Type> resultType = mapType(call->getType(), loc);
  if (failed(resultType))
    return failure();
  if (*resultType == parsed.getType())
    return parsed;
  if (llvm::isa<Float32Type>(*resultType))
    return builder.create<arith::TruncFOp>(loc, *resultType, parsed)
        .getResult();
  return emitError(loc) << "unsupported: strtod result type";
}

FailureOr<Value> CImporter::emitDivCall(const clang::CallExpr *call,
                                        llvm::StringRef name) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 2)
    return emitError(loc) << "unsupported: " << name
                          << " requires exactly 2 arguments";
  // FR-224: THE SURPRISE HERE IS THAT THERE IS NO TYPE PROBLEM. `div_t`
  // is a system-header STRUCT, but the importer has no system-header
  // type rejection at all -- a `div_t` local, a `div_t` return and
  // `.quot`/`.rem` member reads already import today (measured: a
  // hand-written `static div_t mydiv(int,int)` renders a `struct DivT`
  // and both member reads with no patch). Only the CALL was rejected.
  // So this is an ordinary shim, not "system-header type admission".
  //
  // C99 7.20.6.2: quot is numer/denom and rem is numer%denom, with the
  // identity quot*denom+rem == numer -- i.e. C's own truncating-toward-
  // zero `/` and `%`, which are exactly what Rust's `/` and `%` compute
  // on the same signed types and exactly what the importer already emits
  // for a written `a / b`. div(INT_MIN, -1) is UB in both spellings and
  // is refined identically (it is the same `arith.divsi`).
  const clang::FunctionDecl *callee = call->getDirectCallee();
  const auto *record = callee->getReturnType()->getAsRecordDecl();
  if (!record)
    return emitError(loc) << "unsupported: " << name
                          << " does not return a struct";
  const clang::FieldDecl *quotField = nullptr;
  const clang::FieldDecl *remField = nullptr;
  for (const clang::FieldDecl *field : record->fields()) {
    if (field->getName() == "quot")
      quotField = field;
    else if (field->getName() == "rem")
      remField = field;
  }
  // A libc whose div_t spells its members differently, or carries a
  // third one, is not the ISO shape this lowering claims to reproduce.
  if (!quotField || !remField ||
      std::distance(record->field_begin(), record->field_end()) != 2)
    return emitError(loc)
           << "unsupported: " << name
           << " result struct is not the ISO { quot, rem } shape";
  FailureOr<Type> resultType = mapType(callee->getReturnType(), loc);
  if (failed(resultType))
    return failure();
  FailureOr<Type> fieldType = mapType(quotField->getType(), loc);
  if (failed(fieldType))
    return failure();
  auto fieldIntType = llvm::dyn_cast<IntegerType>(*fieldType);
  if (!fieldIntType || fieldIntType.isUnsigned())
    return emitError(loc) << "unsupported: " << name
                          << " result fields must be signed integers";
  FailureOr<Value> numer = emitRValue(call->getArg(0));
  if (failed(numer))
    return failure();
  FailureOr<Value> denom = emitRValue(call->getArg(1));
  if (failed(denom))
    return failure();
  if (!llvm::isa<IntegerType>((*numer).getType()) ||
      !llvm::isa<IntegerType>((*denom).getType()))
    return emitError(loc) << "unsupported: " << name
                          << " arguments must be integers";
  // The prototype has already converted both arguments; normalize the
  // width anyway so a K&R-style declaration cannot produce a mixed
  // divide. BOTH operands are read ONCE into SSA values here and reused
  // by the quotient and the remainder -- `div(f(), g())` must call each
  // exactly once, which a naive `(a/b, a%b)` textual expansion would
  // violate.
  Value n = castToIntType(loc, *numer, fieldIntType);
  Value d = castToIntType(loc, *denom, fieldIntType);
  // No struct-VALUE op exists in the dialect: an aggregate is built as a
  // place plus member assignments, then loaded -- exactly what the
  // importer's own compound-literal path emits, and what the Rust
  // renderer folds back into a `DivT { quot: .., rem: .. }` literal.
  Value place = builder
                    .create<emitrust::VariableOp>(
                        loc, emitrust::LValueType::get(*resultType))
                    .getResult();
  auto assignField = [&](llvm::StringRef fieldName, Value value) {
    Value fieldPlace = builder
                           .create<emitrust::MemberOp>(
                               loc, emitrust::LValueType::get(fieldIntType),
                               place, builder.getStringAttr(fieldName))
                           .getResult();
    builder.create<emitrust::AssignOp>(loc, fieldPlace, value);
  };
  // Both arithmetic ops are materialized up front, in a FIXED order: the
  // alternative (creating each inside its own `assignField` argument)
  // interleaves them with the member ops in whatever order the C++
  // argument evaluation happens to pick, which is not a stable shape to
  // pin a golden against.
  Value quotient = builder.create<arith::DivSIOp>(loc, n, d).getResult();
  Value remainder = builder.create<arith::RemSIOp>(loc, n, d).getResult();
  assignField("quot", quotient);
  assignField("rem", remainder);
  return builder.create<emitrust::LoadOp>(loc, *resultType, place)
      .getResult();
}

LogicalResult CImporter::emitAbortCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 0)
    return emitError(loc) << "unsupported: abort takes no arguments";
  // FR-228. `std::process::abort()` is an exact match for C's abort -- same
  // SIGABRT, same 134 exit status, no destructors, and NO stdout flush --
  // and the last of those is what FR-224 could not admit. Its byte-diff
  // measured:
  //
  //     printf("before abort\n");
  //     abort();
  //
  // clang+glibc with stdout redirected to a FILE writes NOTHING (C11
  // 7.22.4.1p2 leaves the flush implementation-defined and glibc declines);
  // the emitted crate wrote `before abort`, because Rust's `Stdout` is a
  // LineWriter that had already flushed on the newline. The defect was never
  // abort -- it was the BUFFERING MODEL of every emitted crate, and nothing
  // local to this call could fix it (flushing here writes MORE than glibc,
  // not less). FR-228 gave the crate C's model (`emitStdoutRuntime`), so the
  // two now agree: `abort` flushes nothing on either side, and the shape is
  // pinned by test/EndToEnd/stdout-buffering-abort.c against the clang
  // native with stdout redirected to a file.
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("std::process::abort"),
      /*args=*/ArrayAttr(), ValueRange{});
  return success();
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
  // FR-194: C's putchar writes the argument converted to unsigned char --
  // THAT ONE CHARACTER (C11 7.21.7.9), one byte for every value 0..255. The
  // `__emitrust_fmt_c` Display funnel this used to go through is a Latin-1
  // widening to a Unicode scalar, and `Display for char` writes UTF-8, so
  // every byte >= 0x80 came out as two (measured: native `c8 ff`, emitted
  // `c3 88 c3 bf`), silently doubling the length of a `getchar`/`putchar`
  // cat loop over binary data. `__emitrust_byte_out` writes the raw byte on
  // the SAME globally buffered stdout handle `print!` locks, so ordering
  // holds; putchar is a statement position with one argument and no format
  // hole, so no evaluation-order question arises.
  Value byte = castToIntType(loc, *value, builder.getIntegerType(8));
  needsByteOutHelper = true;
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("__emitrust_byte_out"),
      /*args=*/ArrayAttr(), ValueRange{byte});
  return success();
}

std::optional<llvm::StringRef>
CImporter::hostedMathCallee(llvm::StringRef name, bool isFloat) {
  // fabs/sqrt/floor/ceil are IEEE-754-exact (fabs/floor/ceil are exact
  // operations, sqrt is correctly rounded), so every conforming
  // implementation — glibc, Rust's f64 methods, LLVM's constant folder —
  // agrees bit for bit. sin carries no such mandate; it is accepted on
  // the weaker "both sides resolve to the platform libm" argument,
  // pinned differentially (design.md C99-48). exp/log/pow stay rejected
  // (see emitCall) rather than widening that exception.
  //
  // FR-224: the `f`-suffixed forms are the SAME argument at f32. The
  // exactness claim is a property of the OPERATION, not of the width --
  // IEEE-754 mandates fabsf/floorf/ceilf exactly and sqrtf correctly
  // rounded in binary32 by the same clauses that mandate the binary64
  // forms -- so admitting them widens the existing dispatch rather than
  // opening a new exception. `sinf` rides the same platform-libm
  // argument as `sin` and no more. `expf`/`logf`/`powf` stay rejected
  // with `exp`/`log`/`pow`: the suffix changes nothing about libm
  // disagreement (see emitCall).
  if (isFloat)
    return llvm::StringSwitch<std::optional<llvm::StringRef>>(name)
        .Case("sinf", "f32::sin")
        .Case("fabsf", "f32::abs")
        .Case("sqrtf", "f32::sqrt")
        .Case("floorf", "f32::floor")
        .Case("ceilf", "f32::ceil")
        .Default(std::nullopt);
  return llvm::StringSwitch<std::optional<llvm::StringRef>>(name)
      .Case("sin", "f64::sin")
      .Case("fabs", "f64::abs")
      .Case("sqrt", "f64::sqrt")
      .Case("floor", "f64::floor")
      .Case("ceil", "f64::ceil")
      .Default(std::nullopt);
}

FailureOr<Value> CImporter::emitHostedMathCall(const clang::CallExpr *call,
                                               llvm::StringRef rustCallee,
                                               bool isFloat) {
  Location loc = translateLoc(call->getBeginLoc());
  FailureOr<Value> argument = emitRValue(call->getArg(0));
  if (failed(argument))
    return failure();
  // The callee's prototype is `double f(double)` -- or, for the
  // `f`-suffixed forms, `float f(float)` -- checked at the call site, so
  // clang has already converted the argument; anything else indicates an
  // importer bug rather than an unsupported program.
  Type floatType =
      isFloat ? Type(builder.getF32Type()) : Type(builder.getF64Type());
  if ((*argument).getType() != floatType)
    return emitError(loc) << "unsupported: " << rustCallee
                          << " argument is not a "
                          << (isFloat ? "float" : "double");
  return builder
      .create<emitrust::CallOpaqueOp>(loc, TypeRange{floatType},
                                      builder.getStringAttr(rustCallee),
                                      /*args=*/ArrayAttr(),
                                      ValueRange{*argument})
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

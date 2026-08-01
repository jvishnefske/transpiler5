//===- ImportCExpressions.cpp - r-value expression import -------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// CImporter's r-value expression import: emitRValue and its dispatch
/// targets (emitCast, emitBinaryRValue/buildBinaryArith/castToIntType,
/// emitUnaryRValue, emitComparison, emitCondition/emitShortCircuit,
/// emitStmtExpr, emitCall and the indirect/cursor-param/method call-site
/// families, emitEnumConstant/emitEnumOperand/castEnumToI32, and the
/// decomposed/null-pointer-constant classification predicates), plus a
/// CXXBoolLiteralExpr literal case for C++'s `true`/`false` keywords
/// (W2.0). Split out of ImportC.cpp by pure code motion (W1.12); see
/// CImporterInternal.h for the CImporter class declaration this file
/// implements.
//
//===----------------------------------------------------------------------===//

#include "CImporterInternal.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

FailureOr<Value> CImporter::emitRValue(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParens();
  Location loc = translateLoc(e->getBeginLoc());

  // Constant contexts (case values, enumerator initializers) are wrapped in
  // ConstantExpr; translate the wrapped expression. Full expressions
  // containing block-scope compound literals are wrapped in
  // ExprWithCleanups (C99-13); the "cleanup" is the end of the temp's
  // lifetime, which needs no code.
  if (const auto *constant = llvm::dyn_cast<clang::ConstantExpr>(e))
    return emitRValue(constant->getSubExpr());
  if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(e))
    return emitRValue(cleanups->getSubExpr());
  // W2.3: a prvalue bound to a by-value/rvalue-reference parameter (e.g.
  // `v.push_back(1)` binding the literal to `push_back(T&&)`) is wrapped in
  // a `MaterializeTemporaryExpr` marking where its temporary materializes;
  // the value underneath imports exactly like the unwrapped expression (no
  // C construct ever produces this node, so the C path is unaffected).
  if (const auto *materialize =
          llvm::dyn_cast<clang::MaterializeTemporaryExpr>(e))
    return emitRValue(materialize->getSubExpr());
  // An enumerator used as a plain expression has type `int` in C: a named
  // enum's constant is rendered as its Rust variant cast to i32, while an
  // anonymous enum's constant is a plain i32 value.
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e))
    if (const auto *enumerator =
            llvm::dyn_cast<clang::EnumConstantDecl>(ref->getDecl())) {
      FailureOr<Value> constant = emitEnumConstant(enumerator, loc);
      if (failed(constant))
        return failure();
      if (llvm::isa<emitrust::EnumType>((*constant).getType()))
        return castEnumToI32(loc, *constant);
      return constant;
    }
  // W2.3: an lvalue bound to a C++ reference parameter (e.g. `const T&`)
  // is left as a bare `DeclRefExpr` with NO `CK_LValueToRValue` wrapper —
  // binding a reference does not "read" the value, it aliases existing
  // storage — unlike C, where every scalar value use is wrapped in that
  // cast (this branch is unreachable, and so safe, for the C path). A
  // by-value STL method parameter Rust models as an owned value (e.g.
  // `Vec::push`'s `T`) still needs the loaded value, exactly like the
  // `CK_LValueToRValue` case below.
  if (const auto *scalarRef = llvm::dyn_cast<clang::DeclRefExpr>(e))
    if (llvm::isa<clang::VarDecl>(scalarRef->getDecl())) {
      FailureOr<Value> place = emitLValue(scalarRef);
      if (failed(place))
        return failure();
      return loadPlace(loc, *place);
    }

  if (const auto *literal = llvm::dyn_cast<clang::IntegerLiteral>(e)) {
    FailureOr<Type> type = mapType(e->getType(), loc);
    if (failed(type))
      return failure();
    IntegerAttr attr = IntegerAttr::get(*type, literal->getValue());
    // Unsigned literals (42u and friends) cannot be arith constants (arith
    // requires signless types); they become emitrust constants instead.
    if (isUnsignedInt(*type))
      return builder.create<emitrust::ConstantOp>(loc, *type, attr)
          .getResult();
    return builder.create<arith::ConstantOp>(loc, attr).getResult();
  }
  if (const auto *literal = llvm::dyn_cast<clang::FloatingLiteral>(e)) {
    FailureOr<Type> type = mapType(e->getType(), loc);
    if (failed(type))
      return failure();
    // An L-suffixed literal's APFloat carries x87 extended semantics;
    // floatAttrFor narrows it to the f64 the long-double-as-f64 policy
    // substitutes (CTS 00204).
    return builder
        .create<arith::ConstantOp>(
            loc, floatAttrFor(llvm::cast<FloatType>(*type),
                              literal->getValue()))
        .getResult();
  }
  if (const auto *literal = llvm::dyn_cast<clang::CharacterLiteral>(e)) {
    // C99-4 plain-char policy: ordinary character constants have C type
    // int and import as the value clang evaluated; because plain char is
    // signed on the x86-64 Linux target the differential oracle uses, a
    // high-byte constant ('\xff') is stored sign-extended (0xFFFFFFFF,
    // i.e. -1). A wide constant (L'') is also just an int-typed rvalue —
    // wchar_t is int on this target — and imports as its code-point
    // value, matching the wide-string policy of carrying code units
    // verbatim. Unicode constants (u8''/u''/U'') carry charN_t types
    // outside the model and stay rejected.
    clang::CharacterLiteralKind kind = literal->getKind();
    if (kind != clang::CharacterLiteralKind::Ascii &&
        kind != clang::CharacterLiteralKind::Wide)
      return emitError(loc)
             << "unsupported: Unicode character constant (u8'', u'', U'')";
    uint32_t raw = literal->getValue();
    if (kind == clang::CharacterLiteralKind::Ascii) {
      // An ordinary constant must be a single byte: a value in [0,255]
      // or a sign-extended high byte. Anything else is a
      // multi-character constant ('ab'), whose value is
      // implementation-defined and rejected rather than pinned.
      bool singleByte = raw <= 0xFF || (raw & 0xFFFFFF00u) == 0xFFFFFF00u;
      if (!singleByte)
        return emitError(loc) << "unsupported: multi-character constant";
    }
    FailureOr<Type> type = mapType(e->getType(), loc);
    if (failed(type))
      return failure();
    // Materialize the (possibly negative) int-typed value, sign-extended
    // from the stored 32-bit pattern.
    return createIntConstant(loc, *type,
                             static_cast<int64_t>(static_cast<int32_t>(raw)));
  }
  // W2.0: C++'s `true`/`false` are keywords producing a distinct AST node
  // (CXXBoolLiteralExpr), unlike C's stdbool.h macros (`true`/`false` ->
  // plain `1`/`0` IntegerLiteral). Maps to the same i1 constant an
  // equivalent C `_Bool` literal would.
  if (const auto *boolLiteral = llvm::dyn_cast<clang::CXXBoolLiteralExpr>(e))
    return createBoolConstant(loc, boolLiteral->getValue());
  // W2.2: `this` resolves to the raw (undereferenced) receiver argument;
  // every consumer (the `->`-base rvalue path in `emitMemberBasePlace`, in
  // particular) already handles a ref/mut_ref-typed value generically by
  // deref'ing it at the point of use, exactly like any other pointer-typed
  // base expression.
  if (llvm::isa<clang::CXXThisExpr>(e)) {
    if (!currentCxxThisRef)
      return emitError(loc)
             << "unsupported: 'this' outside a non-static member function";
    return currentCxxThisRef;
  }
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e))
    return emitCast(cast);
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e))
    return emitBinaryRValue(binary);
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    return emitUnaryRValue(unary);
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
    FailureOr<Value> result = emitCall(call);
    if (failed(result))
      return failure();
    if (!*result)
      return emitError(loc) << "unsupported: value use of a void call";
    return result;
  }
  if (const auto *conditional = llvm::dyn_cast<clang::ConditionalOperator>(e))
    return emitConditionalOperator(conditional);
  if (const auto *stmtExpr = llvm::dyn_cast<clang::StmtExpr>(e))
    return emitStmtExpr(stmtExpr);
  // `va_arg(ap, T)` inside a monomorphization clone (CTS 00204): a
  // dispatch over the consumption cursor selecting among the clone's
  // extras of static type T.
  if (const auto *vaArg = llvm::dyn_cast<clang::VAArgExpr>(e))
    return emitVaArg(vaArg);
  if (const auto *trait = llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(e))
    return emitSizeofAlignof(trait);
  if (llvm::isa<clang::StringLiteral>(e))
    return emitError(loc)
           << "unsupported: string literal outside a printf format";
  // `__func__` (and __FUNCTION__/__PRETTY_FUNCTION__) is modeled only in
  // the string-literal positions — printf/puts '%s' arguments,
  // char-pointer bindings, and string-helper arguments (C99-29); any
  // other value use keeps a located rejection naming the identifier.
  if (const auto *predefined = llvm::dyn_cast<clang::PredefinedExpr>(e))
    return emitError(loc) << "unsupported use of '"
                          << predefined->getIdentKindName()
                          << "' outside a string literal position";
  // A struct compound literal in value position (assignment right-hand
  // side, by-value argument, return value — contexts where clang does not
  // wrap the aggregate in an lvalue-to-rvalue cast) materializes its
  // anonymous temp and loads it whole (C99-13).
  if (const auto *literal = llvm::dyn_cast<clang::CompoundLiteralExpr>(e)) {
    FailureOr<Value> place = emitCompoundLiteralPlace(literal);
    if (failed(place))
      return failure();
    return loadPlace(loc, *place);
  }
  // A C++ `CXXConstructExpr` in VALUE position — a by-value argument
  // (`f(pt)`), a by-value return (`return r;`), or any other prvalue of class
  // type — that is a TRIVIAL copy or move of an existing object is a
  // whole-struct value copy: unwrap to the single source operand and load it
  // whole, exactly as `x = other;` does. A non-trivial constructor carries
  // real side effects (silently dropping them would be a miscompile, not a
  // merely unsupported construct) and stays rejected, as does a value-position
  // construction with no single source to copy (a default or multi-argument
  // construct); those materialize a temporary only the declaration position
  // (`emitCXXConstructInit`) has a place for.
  if (const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(e)) {
    const clang::CXXConstructorDecl *ctor = construct->getConstructor();
    if (ctor && ctor->isCopyOrMoveConstructor() && ctor->isTrivial() &&
        construct->getNumArgs() == 1)
      return emitRValue(construct->getArg(0));
    return emitError(loc) << "unsupported: constructor in value position "
                             "(only a trivial copy or move is modeled)";
  }
  // An NSDMI (`struct D { int x = 5; };`) surfaces at each use as a
  // `CXXDefaultInitExpr` standing in for the member's in-class initializer;
  // its value is exactly that initializer.
  if (const auto *defaultInit = llvm::dyn_cast<clang::CXXDefaultInitExpr>(e))
    return emitRValue(defaultInit->getExpr());
  return emitError(loc) << "unsupported expression: " << e->getStmtClassName();
}

FailureOr<Value> CImporter::emitCast(const clang::CastExpr *cast) {
  Location loc = translateLoc(cast->getBeginLoc());
  const clang::Expr *sub = cast->getSubExpr();

  switch (cast->getCastKind()) {
  case clang::CK_NoOp:
    return emitRValue(sub);
  case clang::CK_FunctionToPointerDecay:
    // A function used as a value decays to a `Some(name)` fn_ptr constant.
    return emitFunctionPointerConstant(sub, cast->getType(), loc);
  case clang::CK_NullToPointer: {
    // The null constant of a function pointer is `None`. A data-pointer
    // null constant is modeled at its consuming sites (assignment,
    // equality comparison, truth test — the Option-of-cursor model,
    // CTS-P8); one reaching this generic value path has no modeled
    // consumer and keeps its located rejection.
    if (isFunctionPointer(cast->getType())) {
      FailureOr<Type> mapped = mapType(cast->getType(), loc);
      if (failed(mapped))
        return failure();
      return createFnPtrNone(loc, *mapped);
    }
    return emitError(loc) << "unsupported cast ("
                          << cast->getCastKindName() << ")";
  }
  case clang::CK_BitCast: {
    // A conversion between function pointer types, e.g. binding a
    // prototyped function to a prototype-less `int (*)()` pointer. A
    // direct function reference re-resolves against the destination type;
    // any other value is legal only when both sides map to the identical
    // fn_ptr signature (Rust has no function pointer reinterpretation).
    if (isFunctionPointer(cast->getType()) &&
        isFunctionPointer(sub->getType())) {
      const clang::Expr *stripped = stripTrivia(sub);
      if (const auto *decay =
              llvm::dyn_cast<clang::ImplicitCastExpr>(stripped))
        if (decay->getCastKind() == clang::CK_FunctionToPointerDecay)
          return emitFunctionPointerConstant(decay->getSubExpr(),
                                             cast->getType(), loc);
      if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stripped))
        if (unary->getOpcode() == clang::UO_AddrOf)
          return emitFunctionPointerConstant(unary->getSubExpr(),
                                             cast->getType(), loc);
      FailureOr<Value> value = emitRValue(sub);
      if (failed(value))
        return failure();
      FailureOr<Type> mapped = mapType(cast->getType(), loc);
      if (failed(mapped))
        return failure();
      if ((*value).getType() != *mapped)
        return emitError(loc) << "unsupported: function pointer conversion "
                                 "changes the signature";
      return value;
    }
    // CTS-BR (00216): a read of an admitted `void *` fn-ptr member under
    // a cast to its one signature loads the retyped member place — no
    // cast op reaches the IR.
    if (isFunctionPointer(cast->getType())) {
      const auto *memberExpr = llvm::dyn_cast<clang::MemberExpr>(
          sub->IgnoreParenImpCasts());
      const auto *field =
          memberExpr
              ? llvm::dyn_cast<clang::FieldDecl>(memberExpr->getMemberDecl())
              : nullptr;
      if (field && fnPtrMemberTypes.count(field)) {
        FailureOr<Value> place = emitLValue(memberExpr, nullptr);
        if (failed(place))
          return failure();
        Value value = loadPlace(loc, *place);
        FailureOr<Type> mapped = mapType(cast->getType(), loc);
        if (failed(mapped))
          return failure();
        if (value.getType() != *mapped)
          return emitError(loc) << "unsupported: function pointer "
                                   "conversion changes the signature";
        return value;
      }
    }
    return emitError(loc) << "unsupported cast ("
                          << cast->getCastKindName() << ")";
  }
  case clang::CK_LValueToRValue: {
    if (const auto *ref =
            llvm::dyn_cast<clang::DeclRefExpr>(sub->IgnoreParens())) {
      // A pointer parameter read as a value yields its reference SSA value.
      //
      // FR-48 excludes a C++ reference parameter from that shortcut, and
      // the exclusion is the whole semantic difference between the two: a
      // pointer's value IS the borrow (`p` reads the pointer), whereas a
      // reference's value is its REFERENT (`x` reads the pointee). Falling
      // through sends it to the ordinary `emitLValue` + `loadPlace` path,
      // where `emitDeclRefLValue`'s reference branch derefs the borrow
      // first — one `emitrust.deref` plus one `emitrust.load`, exactly
      // what `*p` on the equivalent pointer parameter emits.
      auto it = symbols.find(ref->getDecl());
      if (it != symbols.end() && !isCxxReferenceDecl(ref->getDecl()) &&
          llvm::isa<emitrust::MutRefType, emitrust::RefType>(
              it->second.getType()))
        return it->second;
      // A whole-value read of a global is a direct emitrust.global_load.
      if (const GlobalInfo *global = lookupGlobal(ref->getDecl()))
        return builder
            .create<emitrust::GlobalLoadOp>(loc, global->type,
                                            globalSymbol(global->symbol))
            .getResult();
    }
    // An element read through a cell-slice parameter (CTS-P10) is an
    // `emitrust.cell_get` on the reference itself — no place is staged.
    if (std::optional<CellSliceAccess> access = matchCellSliceAccess(sub))
      return emitCellSliceGet(*access, loc);
    // A wider-than-element view over a byte region (CTS-P11) widens to a
    // `from_ne_bytes` load over sizeof(T) consecutive bytes.
    if (ByteViewDeref wide = classifyByteViewDeref(sub); wide.wideByte) {
      FailureOr<WideByteAccess> access =
          resolveWideByteAccess(wide, loc, /*writeback=*/nullptr);
      if (failed(access))
        return failure();
      return emitWideByteLoad(*access, loc);
    }
    // A bit-field member read is the synthesized mask-and-shift accessor
    // over its backing field (C99-45); no lvalue of the member exists.
    if (const auto *memberExpr =
            llvm::dyn_cast<clang::MemberExpr>(stripTrivia(sub)))
      if (const auto *field =
              llvm::dyn_cast<clang::FieldDecl>(memberExpr->getMemberDecl()))
        if (field->isBitField())
          return emitBitFieldRead(memberExpr, loc);
    FailureOr<Value> place = emitLValue(sub);
    if (failed(place))
      return failure();
    Value loaded = loadPlace(loc, *place);
    // A same-width integer view through a reinterpret-back site
    // (`u = *(unsigned int *)p` over an int base, CTS-P9) loads the base
    // element and bitcasts it to the viewed type: Rust's same-width
    // cross-sign `as` reinterprets the bit pattern, exactly C's
    // effective-type-compatible read.
    if (classifyByteViewDeref(sub).reinterpreted) {
      FailureOr<Type> viewed = mapType(cast->getType(), loc);
      if (failed(viewed))
        return failure();
      if (*viewed != loaded.getType())
        loaded = builder.create<emitrust::CastOp>(loc, *viewed, loaded)
                     .getResult();
    }
    // A union pun arm reads the slot and reinterprets the loaded value
    // as its own (differently-signed) type.
    return reinterpretUnionArmRead(sub, loaded, loc);
  }
  case clang::CK_PointerToIntegral: {
    // `(int) q` of a STATICALLY NULL pointer (a base-less nullable
    // region, CTS-P9) is the integer 0 — no address value ever exists to
    // materialize. Every other pointer-to-int cast keeps its located
    // rejection: a pointer with a real base object would need a genuine
    // address value.
    if (isDataPointer(sub->getType()) && isStaticallyNullPointerExpr(sub)) {
      FailureOr<Type> mapped = mapType(cast->getType(), loc);
      if (failed(mapped))
        return failure();
      if (llvm::isa<IntegerType>(*mapped))
        return createScalarIntConstant(loc, *mapped, 0);
    }
    return emitError(loc) << "unsupported cast ("
                          << cast->getCastKindName() << ")";
  }
  case clang::CK_IntegralCast: {
    // A `sizeof` over a BYTE-REGION aggregate converted to a signed
    // 64-bit context (the `long size` walker parameter) folds directly
    // to the signless constant — the region size is the model's own
    // static fact, and `sizeof(struct W)` stays the FAM-free size
    // (CTS-BR, 00216). Other sizeof shapes keep the historical
    // ui64-constant-plus-cast pair.
    if (const auto *trait = llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(
            sub->IgnoreParenImpCasts());
        trait && trait->getKind() == clang::UETT_SizeOf &&
        !trait->getTypeOfArgument()->isVariablyModifiedType() &&
        isByteRegionAggregate(trait->getTypeOfArgument())) {
      clang::Expr::EvalResult eval;
      Type destType;
      if (FailureOr<Type> mapped = mapType(cast->getType(), loc);
          succeeded(mapped))
        destType = *mapped;
      auto destInt = llvm::dyn_cast_or_null<IntegerType>(destType);
      if (destInt && destInt.isSignless() && destInt.getWidth() == 64 &&
          trait->EvaluateAsInt(eval, astContext()) && !eval.HasSideEffects) {
        // Materialize in the entry block: the region size is a
        // function-wide static fact, and the hoisted constant dominates
        // every use.
        OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToStart(entryBlock);
        return createIntConstant(loc, destInt,
                                 eval.Val.getInt().getSExtValue());
      }
    }
    // Integer-to-enum: a reference to an enumerator of the destination
    // enum keeps the direct constant fast path (in C the enumerator itself
    // has type `int`, so even `enum Color c = Red;` arrives as this cast);
    // any other integer value converts through an `emitrust.cast` to the
    // enum type, rendered as the open enum's value-preserving tuple-struct
    // constructor — C preserves values outside the declared enumerators
    // (C99 6.7.2.2, the object holds any value of the underlying type),
    // and so does the emitted Rust.
    if (const clang::EnumDecl *target = namedEnumDeclOf(cast->getType())) {
      if (const auto *ref =
              llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(sub)))
        if (const auto *enumerator =
                llvm::dyn_cast<clang::EnumConstantDecl>(ref->getDecl()))
          if (llvm::cast<clang::EnumDecl>(enumerator->getDeclContext())
                  ->getDefinition() == target)
            return emitEnumConstant(enumerator, loc);
      FailureOr<Value> value = emitRValue(sub);
      if (failed(value))
        return failure();
      auto sourceType = llvm::dyn_cast<IntegerType>((*value).getType());
      if (!sourceType || sourceType.getWidth() == 1)
        return emitError(loc) << "unsupported: integer to enum conversion";
      FailureOr<Type> mapped = mapType(cast->getType(), loc);
      if (failed(mapped))
        return failure();
      return builder.create<emitrust::CastOp>(loc, *mapped, *value)
          .getResult();
    }
    FailureOr<Value> value = emitRValue(sub);
    if (failed(value))
      return failure();
    // Enum-to-integer (Sema's integral promotion, an arithmetic use, or an
    // explicit cast): an `emitrust.cast` to the mapped destination type
    // (Rust's `as` casts a fieldless `#[repr(i32)]` enum to any integer
    // type via its discriminant, matching C's conversion). Mapping the
    // destination keeps unsigned promotions unsigned, so an enum with an
    // unsigned underlying type meets its comparison or arithmetic partner
    // at the same type and unsigned C semantics are preserved.
    if (llvm::isa<emitrust::EnumType>((*value).getType())) {
      if (!cast->getType().getCanonicalType()->isIntegerType())
        return emitError(loc) << "unsupported integral cast";
      FailureOr<Type> mapped = mapType(cast->getType(), loc);
      if (failed(mapped))
        return failure();
      if (!llvm::isa<IntegerType>(*mapped))
        return emitError(loc) << "unsupported integral cast";
      return builder.create<emitrust::CastOp>(loc, *mapped, *value)
          .getResult();
    }
    FailureOr<Type> mapped = mapType(cast->getType(), loc);
    if (failed(mapped))
      return failure();
    auto sourceType = llvm::dyn_cast<IntegerType>((*value).getType());
    auto targetType = llvm::dyn_cast<IntegerType>(*mapped);
    if (!sourceType || !targetType)
      return emitError(loc) << "unsupported integral cast";
    // A conversion touching an unsigned type on either side is an
    // `emitrust.cast`: Rust's `as` between integer types matches C's
    // conversion semantics exactly (truncation keeps the low bits,
    // widening zero-extends from unsigned and sign-extends from signed,
    // and same-width cross-sign casts reinterpret the bit pattern).
    if (!sourceType.isSignless() || !targetType.isSignless()) {
      if (sourceType == targetType)
        return *value;
      return builder.create<emitrust::CastOp>(loc, targetType, *value)
          .getResult();
    }
    if (sourceType.getWidth() == targetType.getWidth())
      return *value;
    if (sourceType.getWidth() < targetType.getWidth()) {
      if (sourceType.getWidth() == 1)
        return builder.create<arith::ExtUIOp>(loc, targetType, *value)
            .getResult();
      return builder.create<arith::ExtSIOp>(loc, targetType, *value)
          .getResult();
    }
    return builder.create<arith::TruncIOp>(loc, targetType, *value)
        .getResult();
  }
  case clang::CK_IntegralToFloating: {
    FailureOr<Value> value = emitRValue(sub);
    if (failed(value))
      return failure();
    FailureOr<Type> mapped = mapType(cast->getType(), loc);
    if (failed(mapped))
      return failure();
    // Unsigned to float is an `emitrust.cast`: Rust's `u* as f*` performs
    // the same round-to-nearest conversion as C.
    if (isUnsignedInt((*value).getType()))
      return builder.create<emitrust::CastOp>(loc, *mapped, *value)
          .getResult();
    return builder.create<arith::SIToFPOp>(loc, *mapped, *value).getResult();
  }
  case clang::CK_FloatingToIntegral: {
    // A floating value converted directly to an enum destination has no
    // integer intermediary in the AST; the emitrust.cast enum path
    // requires an integer source, so the shape stays rejected.
    if (namedEnumDeclOf(cast->getType()))
      return emitError(loc) << "unsupported: floating to enum conversion";
    FailureOr<Value> value = emitRValue(sub);
    if (failed(value))
      return failure();
    FailureOr<Type> mapped = mapType(cast->getType(), loc);
    if (failed(mapped))
      return failure();
    // Float to unsigned is an `emitrust.cast`. Where the value is in the
    // destination's range the Rust `as` result is identical to C's; out of
    // range C is undefined behavior while Rust `as` saturates, which is an
    // acceptable (defined) refinement of the undefined C behavior.
    if (isUnsignedInt(*mapped))
      return builder.create<emitrust::CastOp>(loc, *mapped, *value)
          .getResult();
    return builder.create<arith::FPToSIOp>(loc, *mapped, *value).getResult();
  }
  case clang::CK_FloatingCast: {
    FailureOr<Value> value = emitRValue(sub);
    if (failed(value))
      return failure();
    FailureOr<Type> mapped = mapType(cast->getType(), loc);
    if (failed(mapped))
      return failure();
    auto sourceType = llvm::dyn_cast<FloatType>((*value).getType());
    auto targetType = llvm::dyn_cast<FloatType>(*mapped);
    if (!sourceType || !targetType)
      return emitError(loc) << "unsupported floating-point cast";
    if (sourceType.getWidth() < targetType.getWidth())
      return builder.create<arith::ExtFOp>(loc, targetType, *value)
          .getResult();
    if (sourceType.getWidth() > targetType.getWidth())
      return builder.create<arith::TruncFOp>(loc, targetType, *value)
          .getResult();
    return *value;
  }
  case clang::CK_IntegralToBoolean: {
    FailureOr<Value> value = emitRValue(sub);
    if (failed(value))
      return failure();
    // An enum tested for truth compares its i32 discriminant against zero.
    Value truth = *value;
    if (llvm::isa<emitrust::EnumType>(truth.getType()))
      truth = castEnumToI32(loc, truth);
    Value zero = createScalarIntConstant(loc, truth.getType(), 0);
    // Unsigned truth tests use `emitrust.cmp` (arith.cmpi requires
    // signless operands; Rust's `!=` is type-directed and hence unsigned).
    if (isUnsignedInt(truth.getType()))
      return builder
          .create<emitrust::CmpOp>(loc, builder.getI1Type(),
                                   emitrust::CmpPredicate::ne, truth, zero)
          .getResult();
    return builder
        .create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne, truth, zero)
        .getResult();
  }
  case clang::CK_FloatingToBoolean: {
    FailureOr<Value> value = emitRValue(sub);
    if (failed(value))
      return failure();
    auto floatType = llvm::cast<FloatType>((*value).getType());
    Value zero =
        builder.create<arith::ConstantOp>(loc, FloatAttr::get(floatType, 0.0))
            .getResult();
    return builder
        .create<arith::CmpFOp>(loc, arith::CmpFPredicate::UNE, *value, zero)
        .getResult();
  }
  default:
    return emitError(loc) << "unsupported cast ("
                          << cast->getCastKindName() << ")";
  }
}

FailureOr<Value> CImporter::emitBinaryRValue(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  clang::BinaryOperatorKind opcode = op->getOpcode();

  // Value-position assignments perform the store and yield the stored
  // value by re-loading the assigned place (C's value of an assignment is
  // the post-assignment value; the load is promoted away by --mem2reg for
  // memref cells).
  if (const auto *compound = llvm::dyn_cast<clang::CompoundAssignOperator>(op)) {
    FailureOr<Value> place = emitCompoundAssignToPlace(compound);
    if (failed(place))
      return failure();
    // A union pun arm's re-loaded slot value reinterprets to the arm's
    // own type, the C type of the assignment expression.
    return reinterpretUnionArmRead(op->getLHS(), loadPlace(loc, *place), loc);
  }
  if (opcode == clang::BO_Assign) {
    FailureOr<Value> place = emitAssignToPlace(op);
    if (failed(place))
      return failure();
    return reinterpretUnionArmRead(op->getLHS(), loadPlace(loc, *place), loc);
  }
  // The comma operator evaluates the left operand for its side effects
  // only and yields the right operand's value.
  if (opcode == clang::BO_Comma) {
    if (failed(emitExprStmt(op->getLHS())))
      return failure();
    return emitRValue(op->getRHS());
  }
  // `p - q` on two pointers into the same object is the plain i64 cursor
  // difference (C's ptrdiff_t is `long`, i.e. i64, on the supported
  // targets); the operands never materialize as pointer values.
  if (opcode == clang::BO_Sub && isPointerType(op->getLHS()->getType()) &&
      isPointerType(op->getRHS()->getType()))
    return emitPointerDifference(op);
  // Any other pointer-valued binary result (`p + 1` in value position) is
  // consumed by `emitPointerRValue` from its dereference or assignment
  // context; a bare one has no scalar representation.
  if (isPointerType(op->getType()))
    return emitError(loc) << "unsupported pointer expression in this context";
  if (op->isComparisonOp()) {
    FailureOr<Value> flag = emitComparison(op);
    if (failed(flag))
      return failure();
    return extendBool(loc, *flag, op->getType());
  }
  if (opcode == clang::BO_LAnd || opcode == clang::BO_LOr) {
    FailureOr<Value> flag = emitCondition(op);
    if (failed(flag))
      return failure();
    return extendBool(loc, *flag, op->getType());
  }

  FailureOr<Value> lhs = emitRValue(op->getLHS());
  if (failed(lhs))
    return failure();
  FailureOr<Value> rhs = emitRValue(op->getRHS());
  if (failed(rhs))
    return failure();
  Value rhsValue = *rhs;
  // A shift amount's C type is promoted independently of the shifted
  // operand's, so the operand types may legitimately differ (`long << int`);
  // normalize the amount to the shifted operand's width.
  auto lhsInt = llvm::dyn_cast<IntegerType>((*lhs).getType());
  auto rhsInt = llvm::dyn_cast<IntegerType>(rhsValue.getType());
  if ((opcode == clang::BO_Shl || opcode == clang::BO_Shr) && lhsInt && rhsInt)
    rhsValue = castToIntType(loc, rhsValue, lhsInt);
  if ((*lhs).getType() != rhsValue.getType())
    return emitError(loc) << "unsupported: binary operand type mismatch";
  return buildBinaryArith(loc, opcode, *lhs, rhsValue);
}

FailureOr<Value> CImporter::buildBinaryArith(Location loc,
                                             clang::BinaryOperatorKind opcode,
                                             Value lhs, Value rhs) {
  if (llvm::isa<FloatType>(lhs.getType())) {
    switch (opcode) {
    case clang::BO_Add:
      return builder.create<arith::AddFOp>(loc, lhs, rhs).getResult();
    case clang::BO_Sub:
      return builder.create<arith::SubFOp>(loc, lhs, rhs).getResult();
    case clang::BO_Mul:
      return builder.create<arith::MulFOp>(loc, lhs, rhs).getResult();
    case clang::BO_Div:
      return builder.create<arith::DivFOp>(loc, lhs, rhs).getResult();
    default:
      return emitError(loc)
             << "unsupported floating-point binary operator '"
             << clang::BinaryOperator::getOpcodeStr(opcode) << "'";
    }
  }
  if (isUnsignedInt(lhs.getType())) {
    // arith ops require signless operands; unsigned arithmetic uses the
    // emitrust binary ops, whose Rust infix operators are type-directed
    // and therefore perform unsigned division, remainder, and comparison.
    switch (opcode) {
    case clang::BO_Add:
      return builder.create<emitrust::AddOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Sub:
      return builder.create<emitrust::SubOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Mul:
      return builder.create<emitrust::MulOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Div:
      return builder.create<emitrust::DivOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Rem:
      return builder.create<emitrust::RemOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_And:
      return builder.create<emitrust::AndOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Or:
      return builder.create<emitrust::OrOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Xor:
      return builder.create<emitrust::XorOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Shl:
      return builder.create<emitrust::ShlOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    case clang::BO_Shr:
      // Rust's `>>` on uN is a logical shift, exactly C's unsigned `>>`.
      return builder.create<emitrust::ShrOp>(loc, lhs.getType(), lhs, rhs)
          .getResult();
    default:
      return emitError(loc) << "unsupported binary operator '"
                            << clang::BinaryOperator::getOpcodeStr(opcode)
                            << "'";
    }
  }
  if (llvm::isa<IntegerType>(lhs.getType())) {
    switch (opcode) {
    case clang::BO_Add:
      return builder.create<arith::AddIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Sub:
      return builder.create<arith::SubIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Mul:
      return builder.create<arith::MulIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Div:
      return builder.create<arith::DivSIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Rem:
      return builder.create<arith::RemSIOp>(loc, lhs, rhs).getResult();
    case clang::BO_And:
      return builder.create<arith::AndIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Or:
      return builder.create<arith::OrIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Xor:
      return builder.create<arith::XOrIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Shl:
      return builder.create<arith::ShLIOp>(loc, lhs, rhs).getResult();
    case clang::BO_Shr:
      // C's `>>` on a signed operand is implementation-defined; every
      // relevant C ABI (and Rust's `>>` on iN, which this lowers to)
      // performs an arithmetic shift, hence shrsi.
      return builder.create<arith::ShRSIOp>(loc, lhs, rhs).getResult();
    default:
      return emitError(loc) << "unsupported binary operator '"
                            << clang::BinaryOperator::getOpcodeStr(opcode)
                            << "'";
    }
  }
  return emitError(loc) << "unsupported binary operand type";
}

Value CImporter::castToIntType(Location loc, Value value,
                               IntegerType target) {
  auto source = llvm::cast<IntegerType>(value.getType());
  if (source == target)
    return value;
  if (!source.isSignless() || !target.isSignless())
    return builder.create<emitrust::CastOp>(loc, target, value).getResult();
  if (source.getWidth() < target.getWidth())
    return builder.create<arith::ExtSIOp>(loc, target, value).getResult();
  return builder.create<arith::TruncIOp>(loc, target, value).getResult();
}

FailureOr<Value> CImporter::emitUnaryRValue(const clang::UnaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  switch (op->getOpcode()) {
  case clang::UO_Plus:
    return emitRValue(op->getSubExpr());
  case clang::UO_Minus: {
    FailureOr<Value> value = emitRValue(op->getSubExpr());
    if (failed(value))
      return failure();
    if (llvm::isa<FloatType>((*value).getType()))
      return builder.create<arith::NegFOp>(loc, *value).getResult();
    // C negates an unsigned value modulo 2^N (C99 6.2.5p9); lower it as
    // `0 - x` through the unsigned emitrust.sub, which translates to Rust's
    // `wrapping_sub` and so matches C's modular semantics on every width
    // instead of panicking on overflow in debug builds.
    if (isUnsignedInt((*value).getType())) {
      Value zero = createScalarIntConstant(loc, (*value).getType(), 0);
      return builder
          .create<emitrust::SubOp>(loc, (*value).getType(), zero, *value)
          .getResult();
    }
    if (llvm::isa<IntegerType>((*value).getType())) {
      Value zero = createIntConstant(loc, (*value).getType(), 0);
      return builder.create<arith::SubIOp>(loc, zero, *value).getResult();
    }
    return emitError(loc) << "unsupported operand of unary '-'";
  }
  case clang::UO_LNot: {
    FailureOr<Value> flag = emitCondition(op->getSubExpr());
    if (failed(flag))
      return failure();
    Value truth = createBoolConstant(loc, true);
    Value inverted =
        builder.create<arith::XOrIOp>(loc, *flag, truth).getResult();
    return extendBool(loc, inverted, op->getType());
  }
  case clang::UO_AddrOf: {
    // `&f` on a function yields the same `Some(f)` constant as the
    // implicit function-to-pointer decay.
    if (isFunctionPointer(op->getType()))
      return emitFunctionPointerConstant(op->getSubExpr(), op->getType(),
                                         loc);
    // A pointer into a global would dangle from the staged local copy the
    // access model uses, so it is rejected until globals get a pointer
    // model.
    if (rootsAtGlobal(op->getSubExpr()))
      return emitError(loc)
             << "unsupported: taking the address of a global variable";
    FailureOr<Value> place = emitLValue(op->getSubExpr());
    if (failed(place))
      return failure();
    auto lvalueType = llvm::dyn_cast<emitrust::LValueType>((*place).getType());
    if (!lvalueType)
      return emitError(loc)
             << "unsupported: cannot take the address of this expression";
    Type refType = emitrust::MutRefType::get(lvalueType.getValueType());
    return builder
        .create<emitrust::AddrOfOp>(loc, refType, *place, /*is_mut=*/true)
        .getResult();
  }
  case clang::UO_PreInc:
  case clang::UO_PostInc:
  case clang::UO_PreDec:
  case clang::UO_PostDec:
    return emitIncDecValue(op);
  case clang::UO_Not: {
    FailureOr<Value> value = emitRValue(op->getSubExpr());
    if (failed(value))
      return failure();
    auto intType = llvm::dyn_cast<IntegerType>((*value).getType());
    if (!intType)
      return emitError(loc) << "unsupported operand of bitwise '~'";
    // `~x` is x XOR all-ones (the -1 bit pattern of the operand's width);
    // unsigned operands use emitrust.xor since arith requires signless.
    if (intType.isUnsigned()) {
      Value allOnes =
          builder
              .create<emitrust::ConstantOp>(
                  loc, intType,
                  IntegerAttr::get(intType,
                                   llvm::APInt::getAllOnes(
                                       intType.getWidth())))
              .getResult();
      return builder
          .create<emitrust::XorOp>(loc, intType, *value, allOnes)
          .getResult();
    }
    Value allOnes = createIntConstant(loc, intType, -1);
    return builder.create<arith::XOrIOp>(loc, *value, allOnes).getResult();
  }
  case clang::UO_Deref:
    // A statement-position dereference of an uncast `void *` (a GNU
    // extension in C) never names an element type at all (CTS-P9); it is
    // rejected rather than silently discarded.
    if (op->getType().getCanonicalType()->isVoidType() &&
        isPointerType(op->getSubExpr()->getType()))
      return emitError(loc) << "unsupported: dereference of a 'void *' "
                               "pointer";
    return emitError(loc) << "unsupported dereference in this context";
  default:
    return emitError(loc) << "unsupported unary operator";
  }
}

FailureOr<Value> CImporter::emitComparison(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());

  // Function pointers are ordinary Option<fn> values; C only defines
  // equality on distinct function pointers, which maps to `emitrust.cmp`
  // on the fn_ptr type (`Option<fn>` derives PartialEq; the null constant
  // arrives as a `None` constant through CK_NullToPointer).
  if (isFunctionPointer(op->getLHS()->getType()) ||
      isFunctionPointer(op->getRHS()->getType())) {
    if (op->getOpcode() != clang::BO_EQ && op->getOpcode() != clang::BO_NE)
      return emitError(loc)
             << "unsupported: ordered comparison of function pointers";
    FailureOr<Value> lhs = emitRValue(op->getLHS());
    if (failed(lhs))
      return failure();
    FailureOr<Value> rhs = emitRValue(op->getRHS());
    if (failed(rhs))
      return failure();
    if ((*lhs).getType() != (*rhs).getType())
      return emitError(loc)
             << "unsupported: comparison operand type mismatch";
    emitrust::CmpPredicate predicate = op->getOpcode() == clang::BO_EQ
                                           ? emitrust::CmpPredicate::eq
                                           : emitrust::CmpPredicate::ne;
    return builder
        .create<emitrust::CmpOp>(loc, builder.getI1Type(), predicate, *lhs,
                                 *rhs)
        .getResult();
  }

  // Pointer comparisons decompose both sides into (base, cursor) pairs;
  // only pointers into the same object have a defined C ordering, and the
  // i64 cursors compare signed. A degenerate side (the address of a
  // scalar) is cursor 0 of its object. Comparisons against a null pointer
  // constant are the Option-of-cursor discrimination (CTS-P8): a string
  // literal or a statically non-null pointer folds (every held address
  // designates a live object, whose address is non-null), and a pointer
  // of a nullable region tests its i1 discriminant.
  if (isPointerType(op->getLHS()->getType()) ||
      isPointerType(op->getRHS()->getType())) {
    // An fgets result compared against a null pointer constant is the
    // end-of-file test of the pinned `while (fgets(...) != NULL)` shape
    // (C99-48): the helper returns -1 for C's NULL result, so the
    // comparison folds to an index test exactly like strchr's below.
    {
      const clang::CallExpr *gets = asHostedFgetsCall(op->getLHS());
      const clang::Expr *nullSide = op->getRHS();
      if (!gets) {
        gets = asHostedFgetsCall(op->getRHS());
        nullSide = op->getLHS();
      }
      if (gets && isNullPointerConstantExpr(nullSide)) {
        if (op->getOpcode() != clang::BO_EQ &&
            op->getOpcode() != clang::BO_NE)
          return emitError(loc) << "unsupported: ordered comparison of an "
                                   "fgets result against a null pointer";
        FailureOr<Value> index = emitFileGetsIndex(gets);
        if (failed(index))
          return failure();
        Value nullIndex =
            createIntConstant(loc, builder.getIntegerType(64), -1);
        arith::CmpIPredicate predicate = op->getOpcode() == clang::BO_EQ
                                             ? arith::CmpIPredicate::eq
                                             : arith::CmpIPredicate::ne;
        return builder
            .create<arith::CmpIOp>(loc, predicate, *index, nullIndex)
            .getResult();
      }
    }
    // A FILE* handle local compared against a null pointer constant is
    // the fopen NULL check (C99-48); `__emitrust_file_ok` is the negation
    // of C's `f == NULL`. Non-handle FILE* comparisons keep the
    // historical pointer machinery and its located rejections.
    {
      auto asHandleRef = [&](const clang::Expr *expr) -> const clang::Expr * {
        if (!isFilePtrType(expr->getType()))
          return nullptr;
        const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
            expr->IgnoreParenImpCasts());
        const auto *var =
            ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
        return var && fileLocals.count(var) ? expr : nullptr;
      };
      const clang::Expr *handleSide = asHandleRef(op->getLHS());
      const clang::Expr *nullSide = op->getRHS();
      if (!handleSide) {
        handleSide = asHandleRef(op->getRHS());
        nullSide = op->getLHS();
      }
      if (handleSide && isNullPointerConstantExpr(nullSide)) {
        if (op->getOpcode() != clang::BO_EQ &&
            op->getOpcode() != clang::BO_NE)
          return emitError(loc) << "unsupported: ordered comparison of a "
                                   "FILE* stream against a null pointer";
        FailureOr<Value> truth = emitFileTruth(handleSide);
        if (failed(truth))
          return failure();
        if (op->getOpcode() == clang::BO_NE)
          return truth;
        Value trueValue = createBoolConstant(loc, true);
        return builder.create<arith::XOrIOp>(loc, *truth, trueValue)
            .getResult();
      }
    }
    bool lhsNull = isNullPointerConstantExpr(op->getLHS());
    bool rhsNull = isNullPointerConstantExpr(op->getRHS());
    auto isLiteralPointer = [](const clang::Expr *expr) {
      return llvm::isa<clang::StringLiteral>(expr->IgnoreParenCasts());
    };
    if ((isLiteralPointer(op->getLHS()) && rhsNull) ||
        (lhsNull && isLiteralPointer(op->getRHS()))) {
      if (op->getOpcode() == clang::BO_EQ)
        return createBoolConstant(loc, false);
      if (op->getOpcode() == clang::BO_NE)
        return createBoolConstant(loc, true);
      return emitError(loc) << "unsupported: ordered comparison of a string "
                               "literal against a null pointer";
    }
    // A strchr/strrchr result compared against a null pointer constant
    // asks "was the byte found": the helpers return the found index or -1
    // for C's NULL result, so the comparison folds to an index test
    // (CTS-L1; 00179.c tests `strrchr(a, 'x') == NULL`). This runs before
    // the general null-comparison path below, which cannot decompose a
    // call expression.
    bool reverse = false;
    const clang::CallExpr *search =
        asHostedStrchrCall(op->getLHS(), reverse);
    const clang::Expr *nullSide = op->getRHS();
    if (!search) {
      search = asHostedStrchrCall(op->getRHS(), reverse);
      nullSide = op->getLHS();
    }
    if (search && isNullPointerConstantExpr(nullSide)) {
      if (op->getOpcode() != clang::BO_EQ && op->getOpcode() != clang::BO_NE)
        return emitError(loc)
               << "unsupported: ordered comparison of a "
               << (reverse ? "strrchr" : "strchr")
               << " result against a null pointer";
      PtrExprValue region;
      FailureOr<Value> index = emitStrchrIndex(search, reverse, region);
      if (failed(index))
        return failure();
      Value notFound =
          createIntConstant(loc, builder.getIntegerType(64), -1);
      arith::CmpIPredicate predicate = op->getOpcode() == clang::BO_EQ
                                           ? arith::CmpIPredicate::eq
                                           : arith::CmpIPredicate::ne;
      return builder
          .create<arith::CmpIOp>(loc, predicate, *index, notFound)
          .getResult();
    }
    if (lhsNull || rhsNull) {
      if (op->getOpcode() != clang::BO_EQ &&
          op->getOpcode() != clang::BO_NE)
        return emitError(loc)
               << "unsupported: ordered comparison against a null pointer";
      bool isEq = op->getOpcode() == clang::BO_EQ;
      if (lhsNull && rhsNull) // `NULL == NULL` is defined and constant.
        return createBoolConstant(loc, isEq);
      // Clang converts the pointer operand to `void *` when the null
      // constant is `(void*)0`; the conversion does not change the
      // decomposition and is peeled here.
      const clang::Expr *pointerSide =
          stripTrivia(lhsNull ? op->getRHS() : op->getLHS());
      while (const auto *cast =
                 llvm::dyn_cast<clang::ImplicitCastExpr>(pointerSide)) {
        if (cast->getCastKind() != clang::CK_BitCast ||
            !isPointerType(cast->getSubExpr()->getType()))
          break;
        pointerSide = stripTrivia(cast->getSubExpr());
      }
      FailureOr<PtrExprValue> pointer = emitPointerRValue(pointerSide);
      if (failed(pointer))
        return failure();
      // A statically-null pointer (base-less nullable region, CTS-P9)
      // decomposes to nothing at all; its null test folds to the constant
      // truth of `null == null`.
      if (!pointer->base && !pointer->literalBacking && !pointer->baseIndex &&
          !pointer->nonNull)
        return createBoolConstant(loc, isEq);
      if (!pointer->nonNull) // Statically non-null: the check folds.
        return createBoolConstant(loc, !isEq);
      if (isEq) {
        Value truth = createBoolConstant(loc, true);
        return builder.create<arith::XOrIOp>(loc, pointer->nonNull, truth)
            .getResult();
      }
      return pointer->nonNull;
    }
    FailureOr<PtrExprValue> lhs = emitPointerRValue(op->getLHS());
    if (failed(lhs))
      return failure();
    FailureOr<PtrExprValue> rhs = emitPointerRValue(op->getRHS());
    if (failed(rhs))
      return failure();
    // `p == q` where either side may be null is defined in C (a null and a
    // non-null pointer compare unequal), but the cursor comparison cannot
    // express it; the mixed-state comparison stays rejected.
    if (lhs->nonNull || rhs->nonNull)
      return emitError(loc)
             << "unsupported: comparison of possibly-null pointers";
    if (lhs->baseIndex || rhs->baseIndex) {
      // A multi-base side compares by (discriminant, cursor) pair: C
      // defines equality across distinct objects (unequal) and within one
      // object (cursor equality), which is exactly the pairwise test.
      // Ordered comparison is only defined within one object, which the
      // static region cannot guarantee here, so it stays rejected.
      if (op->getOpcode() != clang::BO_EQ && op->getOpcode() != clang::BO_NE)
        return emitError(loc) << "unsupported: ordered comparison of "
                                 "pointers bound to multiple objects";
      // Resolve each side to a discriminant value: its own loaded
      // discriminant, or — for a single-base side naming one of the other
      // side's bases (`p == x`) — that base's constant index.
      auto indexOf = [&](const PtrExprValue &side,
                         const PtrExprValue &other) -> Value {
        if (side.baseIndex)
          return side.baseIndex;
        const auto *found = llvm::find(other.multiBases,
                                       PointerBaseKey{side.base, side.member});
        if (!side.base || found == other.multiBases.end())
          return Value();
        return createIntConstant(loc, builder.getIntegerType(32),
                                 found - other.multiBases.begin());
      };
      Value leftIndex = indexOf(*lhs, *rhs);
      Value rightIndex = indexOf(*rhs, *lhs);
      if (!leftIndex || !rightIndex ||
          (lhs->baseIndex && rhs->baseIndex &&
           lhs->multiBases != rhs->multiBases))
        return emitError(loc)
               << "unsupported: comparison of pointers into different "
                  "objects";
      Type indexCursorType = builder.getIntegerType(64);
      Value sameBase = builder
                           .create<arith::CmpIOp>(loc,
                                                  arith::CmpIPredicate::eq,
                                                  leftIndex, rightIndex)
                           .getResult();
      Value leftCursor = lhs->cursor
                             ? lhs->cursor
                             : createIntConstant(loc, indexCursorType, 0);
      Value rightCursor = rhs->cursor
                              ? rhs->cursor
                              : createIntConstant(loc, indexCursorType, 0);
      Value sameCursor = builder
                             .create<arith::CmpIOp>(
                                 loc, arith::CmpIPredicate::eq, leftCursor,
                                 rightCursor)
                             .getResult();
      Value equal =
          builder.create<arith::AndIOp>(loc, sameBase, sameCursor)
              .getResult();
      if (op->getOpcode() == clang::BO_EQ)
        return equal;
      Value truth = createBoolConstant(loc, true);
      return builder.create<arith::XOrIOp>(loc, equal, truth).getResult();
    }
    // Stage 5 (B5, design.md FR-30 follow-on): two pointer locals
    // independently traced back to data-pointer PARAMETERS of the SAME
    // promoted owner method (e.g. `root1 = uf_find(node1); root2 =
    // uf_find(node2);` inside `uf_union`, where `node1`/`node2` are two
    // DIFFERENT parameters) get DIFFERENT `base` identities even though
    // both parameters are, by `planOwners`'s all-or-nothing per-function
    // qualification (every data-pointer parameter of one qualifying
    // method shares one class), provably cursors into the SAME array
    // class (`currentMethodOwner`). A `base` referenced from the body
    // currently being emitted can only be a declaration visible to that
    // body — a parameter of THIS function, a local of THIS function, the
    // owner array itself, or another object — so a pointer parameter
    // found here is, by construction, always a parameter of the current
    // method, never of some other function; no additional
    // cross-function bookkeeping is needed. Narrowly scoped to
    // same-function-body equality/inequality only (never generalized to
    // ordered comparisons or across functions, matching the multi-base
    // carve-out above): when both sides root in the current method's own
    // class this way, the base mismatch is ignored and only the cursor
    // values are compared.
    auto isOwnerClassBase = [&](const clang::VarDecl *base) {
      if (!currentMethodOwner || !base)
        return false;
      if (base == currentMethodOwner)
        return true;
      const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(base);
      return param != nullptr && isPointerType(param->getType()) &&
             !isFunctionPointer(param->getType());
    };
    bool sameOwnerClass =
        lhs->base != rhs->base && !lhs->member && !rhs->member &&
        !lhs->literalBacking && !rhs->literalBacking &&
        (op->getOpcode() == clang::BO_EQ ||
         op->getOpcode() == clang::BO_NE) &&
        isOwnerClassBase(lhs->base) && isOwnerClassBase(rhs->base);
    if (!sameOwnerClass &&
        (lhs->base != rhs->base || lhs->member != rhs->member ||
         lhs->literalBacking != rhs->literalBacking))
      return emitError(loc)
             << "unsupported: comparison of pointers into different objects";
    Type cursorType = builder.getIntegerType(64);
    Value left =
        lhs->cursor ? lhs->cursor : createIntConstant(loc, cursorType, 0);
    Value right =
        rhs->cursor ? rhs->cursor : createIntConstant(loc, cursorType, 0);
    arith::CmpIPredicate predicate;
    switch (op->getOpcode()) {
    case clang::BO_LT:
      predicate = arith::CmpIPredicate::slt;
      break;
    case clang::BO_LE:
      predicate = arith::CmpIPredicate::sle;
      break;
    case clang::BO_GT:
      predicate = arith::CmpIPredicate::sgt;
      break;
    case clang::BO_GE:
      predicate = arith::CmpIPredicate::sge;
      break;
    case clang::BO_EQ:
      predicate = arith::CmpIPredicate::eq;
      break;
    case clang::BO_NE:
      predicate = arith::CmpIPredicate::ne;
      break;
    default:
      return emitError(loc) << "unsupported comparison";
    }
    return builder.create<arith::CmpIOp>(loc, predicate, left, right)
        .getResult();
  }

  // Enum comparisons: Sema promotes enum operands to the (possibly
  // unsigned) underlying type, so the enum values are recovered from behind
  // the promotion casts. Equality maps to `emitrust.cmp` on the enum type
  // (Rust derives PartialEq); relational comparison has no derived Rust
  // ordering and compares the i32 discriminants instead. A comparison
  // between an enum and a non-enum integer takes the generic integer path
  // below: `emitRValue` renders the enum side as its `#[repr(i32)]`
  // discriminant via `emitrust.cast` (Rust `as i32`), matching C's
  // conversion of the enum operand to the common integer type.
  std::optional<EnumOperand> lhsEnum = classifyEnumOperand(op->getLHS());
  std::optional<EnumOperand> rhsEnum = classifyEnumOperand(op->getRHS());
  if (lhsEnum && rhsEnum) {
    if (lhsEnum->decl != rhsEnum->decl)
      return emitError(loc)
             << "unsupported: comparison between distinct enum types";
    FailureOr<Value> lhsValue = emitEnumOperand(*lhsEnum, loc);
    if (failed(lhsValue))
      return failure();
    FailureOr<Value> rhsValue = emitEnumOperand(*rhsEnum, loc);
    if (failed(rhsValue))
      return failure();
    switch (op->getOpcode()) {
    case clang::BO_EQ:
    case clang::BO_NE: {
      emitrust::CmpPredicate predicate = op->getOpcode() == clang::BO_EQ
                                             ? emitrust::CmpPredicate::eq
                                             : emitrust::CmpPredicate::ne;
      return builder
          .create<emitrust::CmpOp>(loc, builder.getI1Type(), predicate,
                                   *lhsValue, *rhsValue)
          .getResult();
    }
    case clang::BO_LT:
    case clang::BO_LE:
    case clang::BO_GT:
    case clang::BO_GE: {
      arith::CmpIPredicate predicate;
      switch (op->getOpcode()) {
      case clang::BO_LT:
        predicate = arith::CmpIPredicate::slt;
        break;
      case clang::BO_LE:
        predicate = arith::CmpIPredicate::sle;
        break;
      case clang::BO_GT:
        predicate = arith::CmpIPredicate::sgt;
        break;
      default:
        predicate = arith::CmpIPredicate::sge;
        break;
      }
      Value lhsInt = castEnumToI32(loc, *lhsValue);
      Value rhsInt = castEnumToI32(loc, *rhsValue);
      return builder.create<arith::CmpIOp>(loc, predicate, lhsInt, rhsInt)
          .getResult();
    }
    default:
      return emitError(loc) << "unsupported comparison";
    }
  }

  // An equality comparison against a negated signed integer literal —
  // `!= EOF` with EOF's `(-1)` expansion in particular — folds the
  // literal to a single negative constant, the pinned C99-48 EOF shape
  // (`arith.constant -1 : i32` met by `arith.cmpi`). Only parens are
  // looked through (an implicit conversion would change the constant's
  // type), and relational comparisons keep the historical `0 - x`
  // negation lowering (pinned in enum-int.c).
  auto emitComparisonOperand =
      [&](const clang::Expr *expr) -> FailureOr<Value> {
    if (op->getOpcode() != clang::BO_EQ && op->getOpcode() != clang::BO_NE)
      return emitRValue(expr);
    const auto *minus =
        llvm::dyn_cast<clang::UnaryOperator>(expr->IgnoreParens());
    if (!minus || minus->getOpcode() != clang::UO_Minus)
      return emitRValue(expr);
    const auto *literal = llvm::dyn_cast<clang::IntegerLiteral>(
        minus->getSubExpr()->IgnoreParens());
    if (!literal)
      return emitRValue(expr);
    Location litLoc = translateLoc(minus->getOperatorLoc());
    FailureOr<Type> mapped = mapType(minus->getType(), litLoc);
    if (failed(mapped))
      return failure();
    auto intType = llvm::dyn_cast<IntegerType>(*mapped);
    if (!intType || !intType.isSignless())
      return emitRValue(expr);
    // The negation wraps in the literal's own width, matching C's
    // int-typed negation of a representable literal.
    llvm::APInt negated = -literal->getValue().sextOrTrunc(intType.getWidth());
    return createIntConstant(litLoc, intType, negated.getSExtValue());
  };
  FailureOr<Value> lhs = emitComparisonOperand(op->getLHS());
  if (failed(lhs))
    return failure();
  FailureOr<Value> rhs = emitComparisonOperand(op->getRHS());
  if (failed(rhs))
    return failure();
  if ((*lhs).getType() != (*rhs).getType())
    return emitError(loc) << "unsupported: comparison operand type mismatch";

  if (llvm::isa<FloatType>((*lhs).getType())) {
    arith::CmpFPredicate predicate;
    switch (op->getOpcode()) {
    case clang::BO_LT:
      predicate = arith::CmpFPredicate::OLT;
      break;
    case clang::BO_LE:
      predicate = arith::CmpFPredicate::OLE;
      break;
    case clang::BO_GT:
      predicate = arith::CmpFPredicate::OGT;
      break;
    case clang::BO_GE:
      predicate = arith::CmpFPredicate::OGE;
      break;
    case clang::BO_EQ:
      predicate = arith::CmpFPredicate::OEQ;
      break;
    case clang::BO_NE:
      predicate = arith::CmpFPredicate::UNE;
      break;
    default:
      return emitError(loc) << "unsupported comparison";
    }
    return builder.create<arith::CmpFOp>(loc, predicate, *lhs, *rhs)
        .getResult();
  }
  if (isUnsignedInt((*lhs).getType())) {
    // arith.cmpi requires signless operands; `emitrust.cmp` renders the
    // Rust infix comparison, which is type-directed and therefore unsigned
    // on uN operands, exactly matching C's unsigned comparisons.
    emitrust::CmpPredicate predicate;
    switch (op->getOpcode()) {
    case clang::BO_LT:
      predicate = emitrust::CmpPredicate::lt;
      break;
    case clang::BO_LE:
      predicate = emitrust::CmpPredicate::le;
      break;
    case clang::BO_GT:
      predicate = emitrust::CmpPredicate::gt;
      break;
    case clang::BO_GE:
      predicate = emitrust::CmpPredicate::ge;
      break;
    case clang::BO_EQ:
      predicate = emitrust::CmpPredicate::eq;
      break;
    case clang::BO_NE:
      predicate = emitrust::CmpPredicate::ne;
      break;
    default:
      return emitError(loc) << "unsupported comparison";
    }
    return builder
        .create<emitrust::CmpOp>(loc, builder.getI1Type(), predicate, *lhs,
                                 *rhs)
        .getResult();
  }
  if (llvm::isa<IntegerType>((*lhs).getType())) {
    arith::CmpIPredicate predicate;
    switch (op->getOpcode()) {
    case clang::BO_LT:
      predicate = arith::CmpIPredicate::slt;
      break;
    case clang::BO_LE:
      predicate = arith::CmpIPredicate::sle;
      break;
    case clang::BO_GT:
      predicate = arith::CmpIPredicate::sgt;
      break;
    case clang::BO_GE:
      predicate = arith::CmpIPredicate::sge;
      break;
    case clang::BO_EQ:
      predicate = arith::CmpIPredicate::eq;
      break;
    case clang::BO_NE:
      predicate = arith::CmpIPredicate::ne;
      break;
    default:
      return emitError(loc) << "unsupported comparison";
    }
    return builder.create<arith::CmpIOp>(loc, predicate, *lhs, *rhs)
        .getResult();
  }
  return emitError(loc) << "unsupported comparison operand type";
}

FailureOr<Value> CImporter::emitCondition(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParens();
  Location loc = translateLoc(e->getBeginLoc());

  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e)) {
    if (binary->isComparisonOp())
      return emitComparison(binary);
    if (binary->getOpcode() == clang::BO_LAnd ||
        binary->getOpcode() == clang::BO_LOr)
      return emitShortCircuit(binary);
  }
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    if (unary->getOpcode() == clang::UO_LNot) {
      FailureOr<Value> inner = emitCondition(unary->getSubExpr());
      if (failed(inner))
        return failure();
      Value truth = createBoolConstant(loc, true);
      return builder.create<arith::XOrIOp>(loc, *inner, truth).getResult();
    }
  // A function pointer tested for truth (`if (fp)`, `!fp`) is a null
  // check: load the Option<fn> value and compare it against a `None`
  // constant. Clang leaves the condition fn-ptr-typed (no boolean cast).
  if (isFunctionPointer(e->getType())) {
    FailureOr<Value> value = emitRValue(e);
    if (failed(value))
      return failure();
    Value none = createFnPtrNone(loc, (*value).getType());
    return builder
        .create<emitrust::CmpOp>(loc, builder.getI1Type(),
                                 emitrust::CmpPredicate::ne, *value, none)
        .getResult();
  }
  // A FILE* handle local tested for truth (`if (!f)` after fopen —
  // C99-48) is its NULL check, and a truth-tested fgets result is its
  // end-of-file test; both must run before the decomposed-pointer truth
  // paths below, which cannot represent a stream. Non-handle FILE*
  // expressions (`if (stdin)`) keep the historical paths and their
  // located rejections.
  auto isFileTruthExpr = [&](const clang::Expr *expr) {
    if (asHostedFgetsCall(expr))
      return true;
    const auto *ref =
        llvm::dyn_cast<clang::DeclRefExpr>(expr->IgnoreParenImpCasts());
    const auto *var =
        ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
    return var && fileLocals.count(var);
  };
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_PointerToBoolean &&
        isFileTruthExpr(cast->getSubExpr()))
      return emitFileTruth(cast->getSubExpr());
  if (isFileTruthExpr(e) &&
      (isFilePtrType(e->getType()) || asHostedFgetsCall(e)))
    return emitFileTruth(e);
  // A data pointer tested for truth (`if (p)`, `!p`) is a null check: the
  // Option-of-cursor discrimination of the decomposed pointer (CTS-P8).
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_PointerToBoolean)
      return emitPointerTruth(cast->getSubExpr());
  if (isPointerType(e->getType()))
    return emitPointerTruth(e);
  // Strip explicit truthiness casts so `_Bool` conversions don't double up.
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_IntegralToBoolean ||
        cast->getCastKind() == clang::CK_FloatingToBoolean)
      return emitCondition(cast->getSubExpr());

  FailureOr<Value> value = emitRValue(e);
  if (failed(value))
    return failure();
  Type type = (*value).getType();
  if (type.isInteger(1))
    return *value;
  // C tests an enum for truth by its integer value; compare the i32
  // discriminant against zero.
  if (llvm::isa<emitrust::EnumType>(type)) {
    Value discriminant = castEnumToI32(loc, *value);
    Value zero = createIntConstant(loc, builder.getI32Type(), 0);
    return builder
        .create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne, discriminant,
                               zero)
        .getResult();
  }
  // An unsigned value is truthy when not equal to an unsigned zero; the
  // comparison must be an `emitrust.cmp` (arith requires signless).
  if (isUnsignedInt(type)) {
    Value zero = createScalarIntConstant(loc, type, 0);
    return builder
        .create<emitrust::CmpOp>(loc, builder.getI1Type(),
                                 emitrust::CmpPredicate::ne, *value, zero)
        .getResult();
  }
  if (llvm::isa<IntegerType>(type)) {
    Value zero = createIntConstant(loc, type, 0);
    return builder
        .create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne, *value, zero)
        .getResult();
  }
  if (auto floatType = llvm::dyn_cast<FloatType>(type)) {
    Value zero =
        builder.create<arith::ConstantOp>(loc, FloatAttr::get(floatType, 0.0))
            .getResult();
    return builder
        .create<arith::CmpFOp>(loc, arith::CmpFPredicate::UNE, *value, zero)
        .getResult();
  }
  return emitError(loc) << "unsupported condition type";
}

FailureOr<Value> CImporter::emitShortCircuit(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  bool isAnd = op->getOpcode() == clang::BO_LAnd;

  // A compile-time-constant left operand decides the circuit BEFORE
  // lowering (mirroring `emitConditionalOperator`): when it
  // short-circuits, C guarantees the right operand never evaluates, so a
  // dead RHS may contain otherwise-unimportable constructs (the 00207
  // `0 && printf(...)` value shape); when it does not, the RHS alone
  // decides the truth value. A live label in the dead operand keeps the
  // full lowering below (see `emitConditionalOperator`).
  clang::Expr::EvalResult lhsValue;
  if (op->getLHS()->EvaluateAsInt(lhsValue, astContext())) {
    bool truth = lhsValue.Val.getInt() != 0;
    if (truth == isAnd) // `1 && rhs` / `0 || rhs`: the RHS decides.
      return emitCondition(op->getRHS());
    if (!containsLabelStmt(op->getRHS()) &&
        !findNestedSwitchLabel(op->getRHS()))
      return createIntConstant(loc, builder.getI1Type(), truth ? 1 : 0);
  }

  // The result lives in a promotable rank-0 i1 cell: the left value covers
  // the short-circuit path, the right value overwrites it otherwise.
  Value flag = createEntryAlloca(loc, builder.getI1Type());
  FailureOr<Value> lhs = emitCondition(op->getLHS());
  if (failed(lhs))
    return failure();
  builder.create<memref::StoreOp>(loc, *lhs, flag);

  Block *rhsBlock = createBlock();
  Block *endBlock = createBlock();
  if (isAnd)
    builder.create<cf::CondBranchOp>(loc, *lhs, rhsBlock, ValueRange(),
                                     endBlock, ValueRange());
  else
    builder.create<cf::CondBranchOp>(loc, *lhs, endBlock, ValueRange(),
                                     rhsBlock, ValueRange());

  builder.setInsertionPointToEnd(rhsBlock);
  FailureOr<Value> rhs = emitCondition(op->getRHS());
  if (failed(rhs))
    return failure();
  builder.create<memref::StoreOp>(loc, *rhs, flag);
  builder.create<cf::BranchOp>(loc, endBlock);

  builder.setInsertionPointToEnd(endBlock);
  return builder.create<memref::LoadOp>(loc, flag, ValueRange()).getResult();
}

FailureOr<Value>
CImporter::emitConditionalOperator(const clang::ConditionalOperator *op) {
  Location loc = translateLoc(op->getQuestionLoc());
  // A compile-time-constant, side-effect-free condition elides the dead
  // arm BEFORE lowering (mirroring `emitIfStmt`), so a dead arm may
  // contain otherwise-unimportable constructs. The elision is gated on
  // the same live-label check as the if form: a goto-targeted label in
  // the dead arm (necessarily inside a statement expression, and
  // necessarily targeted from within the arm itself — clang rejects
  // jumps into a statement expression from outside) keeps the arm's code
  // reachable, and a case/default label of an enclosing switch must not
  // be dropped either — both shapes keep the FULL lowering below, whose
  // constant branch leaves the arm dynamically dead while its labels
  // register with the ordinary dispatch (the 00213 kb_wait_1 shape).
  clang::Expr::EvalResult conditionValue;
  if (op->getCond()->EvaluateAsInt(conditionValue, astContext())) {
    bool truth = conditionValue.Val.getInt() != 0;
    const clang::Expr *live = truth ? op->getTrueExpr() : op->getFalseExpr();
    const clang::Expr *dead = truth ? op->getFalseExpr() : op->getTrueExpr();
    if (!containsLabelStmt(dead) && !findNestedSwitchLabel(dead))
      return emitRValue(live);
  }
  FailureOr<Type> mapped = mapType(op->getType(), loc);
  if (failed(mapped))
    return failure();
  if (!llvm::isa<IntegerType, FloatType>(*mapped))
    return emitError(loc)
           << "unsupported: conditional operator on a non-scalar operand";

  // The result flows through a cell so that each arm evaluates in its own
  // block and only the selected arm's side effects run: a promotable
  // rank-0 memref cell for memref-legal scalars, or an `emitrust.variable`
  // place for unsigned integers (see `emitLocalVar` for why unsigned
  // values cannot live in memref cells).
  Value cell = isUnsignedInt(*mapped)
                   ? builder
                         .create<emitrust::VariableOp>(
                             loc, emitrust::LValueType::get(*mapped))
                         .getResult()
                   : createEntryAlloca(loc, *mapped);

  FailureOr<Value> condition = emitCondition(op->getCond());
  if (failed(condition))
    return failure();
  Block *trueBlock = createBlock();
  Block *falseBlock = createBlock();
  Block *endBlock = createBlock();
  builder.create<cf::CondBranchOp>(loc, *condition, trueBlock, ValueRange(),
                                   falseBlock, ValueRange());

  // Clang has already wrapped both arms in the implicit conversions to the
  // operator's common type, so after mapping both arms must produce
  // exactly the cell's type; anything else is an importer gap and is
  // rejected rather than silently converted.
  auto emitArm = [&](Block *block, const clang::Expr *arm) -> LogicalResult {
    builder.setInsertionPointToEnd(block);
    FailureOr<Value> value = emitRValue(arm);
    if (failed(value))
      return failure();
    if ((*value).getType() != *mapped)
      return emitError(translateLoc(arm->getBeginLoc()))
             << "unsupported: conditional operator arm type mismatch";
    if (failed(storeToPlace(loc, cell, *value)))
      return failure();
    // The arm's expression may have opened further blocks (nested `?:` or
    // short-circuit operators); the branch closes the current one.
    builder.create<cf::BranchOp>(loc, endBlock);
    return success();
  };
  if (failed(emitArm(trueBlock, op->getTrueExpr())) ||
      failed(emitArm(falseBlock, op->getFalseExpr())))
    return failure();

  builder.setInsertionPointToEnd(endBlock);
  return loadPlace(loc, cell);
}

/// Finds a `goto` below `expr`'s body whose target label is NOT declared
/// inside the same statement expression, or null when every goto stays
/// internal. Labels of nested statement expressions count as internal:
/// clang itself rejects any jump INTO a statement expression, so a goto
/// this scan accepts always lands on a label the flattened body registers.
static const clang::GotoStmt *
findGotoOutOfStmtExpr(const clang::StmtExpr *expr) {
  llvm::SmallPtrSet<const clang::LabelDecl *, 8> internalLabels;
  SmallVector<const clang::Stmt *> worklist{expr->getSubStmt()};
  while (!worklist.empty()) {
    const clang::Stmt *current = worklist.pop_back_val();
    if (!current)
      continue;
    if (const auto *label = llvm::dyn_cast<clang::LabelStmt>(current))
      internalLabels.insert(label->getDecl());
    for (const clang::Stmt *child : current->children())
      worklist.push_back(child);
  }
  worklist.push_back(expr->getSubStmt());
  while (!worklist.empty()) {
    const clang::Stmt *current = worklist.pop_back_val();
    if (!current)
      continue;
    if (const auto *gotoStmt = llvm::dyn_cast<clang::GotoStmt>(current))
      if (!internalLabels.contains(gotoStmt->getLabel()))
        return gotoStmt;
    for (const clang::Stmt *child : current->children())
      worklist.push_back(child);
  }
  return nullptr;
}

FailureOr<Value> CImporter::emitStmtExpr(const clang::StmtExpr *expr) {
  Location loc = translateLoc(expr->getBeginLoc());
  // A goto that leaves a value-position statement expression abandons the
  // expression mid-evaluation: the synthesized value temp would never be
  // written and the consumer would read garbage.
  if (const clang::GotoStmt *escape = findGotoOutOfStmtExpr(expr))
    return emitError(translateLoc(escape->getGotoLoc()))
           << "unsupported: goto out of a statement expression in value "
              "position";
  FailureOr<Type> mapped = mapType(expr->getType(), loc);
  if (failed(mapped))
    return failure();
  if (!llvm::isa<IntegerType, FloatType>(*mapped))
    return emitError(loc)
           << "unsupported: statement expression of a non-scalar type";
  const clang::CompoundStmt *body = expr->getSubStmt();
  const clang::Expr *valueExpr =
      body->body_empty() ? nullptr
                         : llvm::dyn_cast<clang::Expr>(body->body_back());
  if (!valueExpr) // Defensive: clang typed this StmtExpr non-void.
    return emitError(loc) << "unsupported: statement expression without a "
                             "final expression statement";
  // The value transits a synthesized temp cell so that the body statements
  // — which lower FLATTENED into the enclosing function and may open
  // further blocks (labels, loops, nested conditionals) — never wall the
  // value off in a region: an `emitrust.variable` place for unsigned
  // integers, a promotable rank-0 memref cell otherwise (mirroring
  // `emitConditionalOperator`).
  Value cell = isUnsignedInt(*mapped)
                   ? builder
                         .create<emitrust::VariableOp>(
                             loc, emitrust::LValueType::get(*mapped))
                         .getResult()
                   : createEntryAlloca(loc, *mapped);
  for (const clang::Stmt *child : body->body()) {
    if (child == body->body_back())
      break;
    if (failed(emitStmt(child)))
      return failure();
  }
  // C applies the lvalue conversion to the final expression statement;
  // clang usually materializes it, but a bare lvalue is loaded here so
  // both AST shapes land the same value in the temp.
  FailureOr<Value> value = failure();
  if (valueExpr->isGLValue()) {
    FailureOr<Value> place = emitLValue(valueExpr);
    if (failed(place))
      return failure();
    value = loadPlace(loc, *place);
  } else {
    value = emitRValue(valueExpr);
  }
  if (failed(value))
    return failure();
  if ((*value).getType() != *mapped)
    return emitError(translateLoc(valueExpr->getBeginLoc()))
           << "unsupported: statement expression value type mismatch";
  if (failed(storeToPlace(loc, cell, *value)))
    return failure();
  return loadPlace(loc, cell);
}

FailureOr<Value>
CImporter::emitSizeofAlignof(const clang::UnaryExprOrTypeTraitExpr *expr) {
  Location loc = translateLoc(expr->getBeginLoc());
  clang::UnaryExprOrTypeTrait kind = expr->getKind();
  if (kind != clang::UETT_SizeOf && kind != clang::UETT_AlignOf)
    return emitError(loc) << "unsupported sizeof/alignof kind";
  clang::QualType operand = expr->isArgumentType()
                                ? expr->getArgumentType()
                                : expr->getArgumentExpr()->getType();
  // `sizeof` of a variable-length array is the one operand C evaluates at
  // run time; there is no constant to fold. Incomplete and function types
  // (GNU extensions accept them) have no portable layout either.
  if (operand->isVariablyModifiedType())
    return emitError(loc)
           << "unsupported: sizeof/alignof of a variable-length array";
  if (operand->isIncompleteType() || operand->isFunctionType())
    return emitError(loc)
           << "unsupported: sizeof/alignof of an incomplete or function type";
  // C99-45: the backing-run model gives a bit-field struct a well-defined
  // Rust size, but that size need not match the C ABI layout this fold
  // would promise, so the importer refuses rather than asserting
  // ABI-exact layout.
  if (typeContainsBitField(operand))
    return emitError(loc)
           << (kind == clang::UETT_SizeOf
                   ? "unsupported: sizeof of a struct with bit-fields"
                   : "unsupported: alignof of a struct with bit-fields");
  // CTS 00204: long double imports as an 8-byte f64, but the C fold would
  // yield the 16-byte x86-64 ABI size — a layout the emitted Rust never
  // keeps (the same reasoning as the bit-field rejection above).
  if (typeContainsLongDouble(operand))
    return emitError(loc) << "unsupported: sizeof/alignof of long double";
  int64_t value =
      kind == clang::UETT_SizeOf
          ? astContext().getTypeSizeInChars(operand).getQuantity()
          : astContext().getTypeAlignInChars(operand).getQuantity();
  // The result's C type is size_t (unsigned long here); the fold uses
  // clang's target layout, so the value always matches a native build of
  // the same translation unit.
  FailureOr<Type> resultType = mapType(expr->getType(), loc);
  if (failed(resultType))
    return failure();
  return createScalarIntConstant(loc, *resultType, value);
}

FailureOr<Value> CImporter::emitCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  // W2.2: a genuine C++ instance-method call (`obj.method(args)` or
  // `obj->method(args)`) lowers to `emitrust.method_call` on the (possibly
  // const) receiver place — intercepted before the ordinary free-function
  // dispatch below, which a `CXXMemberCallExpr` (itself a `CallExpr`) would
  // otherwise reach.
  if (const auto *memberCall = llvm::dyn_cast<clang::CXXMemberCallExpr>(call))
    return emitCXXMemberCall(memberCall);
  // W2.3: `v[i]` / `s1 += s2` / `s += 'c'` over a recognized STL receiver
  // resolve to an overloaded operator call (`CXXOperatorCallExpr`, itself a
  // `CallExpr` but NOT a `CXXMemberCallExpr` — clang represents even a
  // MEMBER `operator[]`/`operator+=` this way for the operator syntax) —
  // intercepted before the ordinary free-function dispatch below, which
  // would otherwise reject it as a call to an unimported function.
  if (const auto *opCall = llvm::dyn_cast<clang::CXXOperatorCallExpr>(call)) {
    const auto *opMethod = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(
        opCall->getDirectCallee());
    if (opMethod && opMethod->getParent()->isInStdNamespace())
      return emitStlOperatorCall(opCall);
  }
  const clang::FunctionDecl *callee = call->getDirectCallee();
  if (!callee)
    return emitIndirectCall(call);
  // `__builtin_expect(e, c)` is a pure branch-prediction hint: it folds to
  // its first argument during import, in every position (condition or
  // value), so no call op or `__builtin_expect` symbol ever reaches the
  // IR. A constant argument (`!!(0)`) then composes with the
  // constant-condition dead-arm elision of `emitIfStmt` and
  // `emitConditionalOperator` through clang's constant evaluator, which
  // folds the intact call the same way.
  switch (callee->getBuiltinID()) {
  case clang::Builtin::BI__builtin_expect:
  case clang::Builtin::BI__builtin_expect_with_probability:
    return emitRValue(call->getArg(0));
  default:
    break;
  }
  if (!callee->getDeclName().isIdentifier())
    return emitError(loc) << "unsupported callee";
  // The hosted (definition-less) printf lowering is statement-position
  // only; a project-supplied printf definition is an ordinary imported
  // function whose result is an ordinary value in every position.
  if (callee->getName() == "printf" && !callee->getDefinition())
    return emitError(loc) << "unsupported: printf return value must be unused";
  // Statement-position puts/putchar are lowered by name (emitCallStmt);
  // their int result has no representation there, so a value use of a
  // definition-less puts/putchar is rejected.
  if ((callee->getName() == "puts" || callee->getName() == "putchar") &&
      !callee->getDefinition())
    return emitError(loc) << "unsupported: " << callee->getName()
                          << " return value must be unused";
  // A definition-less strlen is lowered by name like printf/puts: its
  // supported argument shape is a pointer into a string-literal region,
  // rendered through the `__emitrust_strlen` helper. A user-defined strlen
  // is an ordinary call.
  if (callee->getName() == "strlen" && !callee->getDefinition())
    return emitStrlenCall(call);
  // Hosted <string.h> comparisons (design.md C99-48, CTS-L1) are lowered
  // by name, like strlen; their int result is an ordinary value. The
  // copy/fill functions of the same surface are statement-position only
  // (their char* result has no decomposed representation), and a
  // strchr/strrchr result is consumed by the printf %s and null-comparison
  // interceptions before reaching this point.
  if (!callee->getDefinition()) {
    llvm::StringRef name = callee->getName();
    // A definition-less sprintf is lowered by name (design.md CTS-P9,
    // 00186): the literal format translates through the shared printf
    // grammar into a `format!` String and the `__emitrust_sprintf`
    // helper writes it into the destination char region, returning the
    // length. Statement-position calls arrive here through emitCallStmt's
    // emitCall fallthrough and simply discard the value.
    if (name == "sprintf")
      return emitSprintf(call);
    if (name == "strcmp")
      return emitStringCompareCall(call, "strcmp", /*hasCount=*/false);
    if (name == "strncmp")
      return emitStringCompareCall(call, "strncmp", /*hasCount=*/true);
    if (name == "memcmp")
      return emitStringCompareCall(call, "memcmp", /*hasCount=*/true);
    if (name == "strcpy" || name == "strncpy" || name == "strcat" ||
        name == "memset" || name == "memcpy" || name == "memmove")
      return emitError(loc) << "unsupported: " << name
                            << " return value must be unused";
    // Hosted <stdlib.h> value-position surface (design.md C99-48):
    // abs/labs onto wrapping_abs, atoi as an exact C parse over the
    // argument's char region. A user-defined function of any of these
    // names is an ordinary call (the getDefinition guard above).
    if (name == "abs")
      return emitAbsCall(call, /*isLong=*/false);
    if (name == "labs")
      return emitAbsCall(call, /*isLong=*/true);
    if (name == "atoi")
      return emitAtoiCall(call);
    // Curated <math.h> rejections (design.md C99-48): C imposes no
    // accuracy requirement on these, implementations disagree in the
    // last bits, and rustc may constant-fold them through a libm other
    // than the differential oracle's — no bit-exact safe-Rust mapping
    // can be argued, so they stay out of the curated subset by policy
    // (a sharper diagnostic than the generic system-header rejection).
    if (name == "pow" || name == "exp" || name == "log")
      return emitError(loc)
             << "unsupported: '" << name
             << "' has no bit-exact Rust mapping (C imposes no accuracy "
                "requirement and libm implementations disagree)";
    if (name == "strchr" || name == "strrchr")
      return emitError(loc)
             << "unsupported: a " << name
             << " result must feed a printf '%s' argument or a "
                "comparison against a null pointer";
    // Hosted <stdio.h> FILE* streams (design.md C99-48, CTS-T1.3):
    // sequential byte-wise I/O on an owned function-local handle. File
    // positioning contradicts the sequential-only model and stays a
    // located rejection.
    if (name == "fseek" || name == "ftell" || name == "rewind")
      return emitError(loc) << "unsupported: file positioning '" << name
                            << "' (FILE* streams are sequential-only)";
    // fprintf to a real stream variable would need a formatted writer
    // over the handle; only the devirtualized stdout-swallow form (see
    // emitAliasedPrintf) is accepted. The literal `stdout` argument
    // falls through to the historical variadic-call rejection.
    if (name == "fprintf" && call->getNumArgs() >= 1) {
      const auto *stream = llvm::dyn_cast<clang::DeclRefExpr>(
          call->getArg(0)->IgnoreParenImpCasts());
      const clang::NamedDecl *streamDecl =
          stream ? llvm::dyn_cast<clang::NamedDecl>(stream->getDecl())
                 : nullptr;
      if (!streamDecl || !streamDecl->getDeclName().isIdentifier() ||
          streamDecl->getName() != "stdout")
        return emitError(translateLoc(call->getArg(0)->getBeginLoc()))
               << "unsupported: fprintf to a FILE* stream (only the "
                  "devirtualized stdout form is supported)";
    }
    // A fopen result in any position but a FILE* handle local's
    // initializer or assignment (consumed by emitFileLocal /
    // emitFileOpenInto before this point) falls through to the
    // system-header rejection below, keeping the historical wording.
    if (name == "fgetc" || name == "getc")
      return emitFileGetc(call);
    if (name == "fread")
      return emitFileReadWrite(call, /*isWrite=*/false);
    if (name == "fwrite")
      return emitFileReadWrite(call, /*isWrite=*/true);
    // An fgets result is consumed by the null-comparison and truth-test
    // interceptions (emitComparison / emitCondition); its char* value
    // has no representation anywhere else.
    if (name == "fgets")
      return emitError(loc)
             << "unsupported: an fgets result must be compared against a "
                "null pointer or tested for truth";
    // Statement-position fclose is lowered by emitCallStmt; C's int
    // result has no representation here.
    if (name == "fclose")
      return emitError(loc)
             << "unsupported: fclose return value must be unused";
  }
  // A variadic callee is supported when its definition imports as its
  // fixed prototype (a va_list-free body, see `importFunction`; the call
  // drops its trailing extras below) or when it monomorphizes per call
  // site (a bounded va_list-using body, CTS 00204; the call rewrites to
  // this site's clone with the extras as ordinary fixed arguments).
  // Every other variadic call keeps the rejection.
  const VaClonePlan *vaClone = nullptr;
  if (callee->isVariadic()) {
    const clang::FunctionDecl *definition = callee->getDefinition();
    auto planIt = definition
                      ? vaMonomorphPlans.find(definition->getCanonicalDecl())
                      : vaMonomorphPlans.end();
    if (planIt != vaMonomorphPlans.end()) {
      auto siteIt = vaCallSiteClones.find(call);
      if (siteIt == vaCallSiteClones.end()) // Defensive; the planner
                                            // enumerated every site.
        return emitError(loc) << "unsupported: call to a variadic function";
      vaClone = &planIt->second.clones[siteIt->second];
    } else if (!definition &&
               crossTuVaListVariadicNames.contains(mlirFuncName(callee))) {
      // W3.0: no definition is visible in THIS TU, but the project-wide
      // pre-scan (`collectCrossTuVaListVariadics`) found a va_list-using
      // definition of this same symbol in another TU. `planVaMonomorph`
      // only enumerates call sites within a single TU, so this site was
      // never assigned a clone and the definition's own TU never emits an
      // ordinary symbol for it either (a va_list-monomorphized definition
      // is ALWAYS replaced by its per-site clones, never kept as a plain
      // function) — without this check the call would reference a symbol
      // that exists nowhere in the module, an unresolved cross-TU call
      // escaping with no diagnostic. Real cross-TU monomorphization
      // support is future work (W3.5); for now this is a located
      // rejection instead.
      return emitError(loc)
             << "unsupported: call to a variadic function '"
             << callee->getName()
             << "' defined in another translation unit";
    } else if (!definition || !definition->hasBody() ||
               bodyUsesVaList(astContext(), definition->getBody())) {
      return emitError(loc) << "unsupported: call to a variadic function";
    }
  }

  // Hosted <math.h> surface (design.md C99-48): a definition-less call to
  // a recognized math function with its standard `double f(double)`
  // prototype lowers to the equivalent safe Rust f64 function. A
  // user-defined function of the same name stays an ordinary call
  // (mirroring the puts/putchar policy), and every other system-header
  // call keeps the rejection below.
  if (!callee->getDefinition() && callee->getNumParams() == 1 &&
      astContext().hasSameUnqualifiedType(callee->getReturnType(),
                                          astContext().DoubleTy) &&
      astContext().hasSameUnqualifiedType(callee->getParamDecl(0)->getType(),
                                          astContext().DoubleTy)) {
    if (std::optional<llvm::StringRef> rustCallee =
            hostedMathCallee(callee->getName()))
      return emitHostedMathCall(call, *rustCallee);
  }

  // W2.2: the only `CXXMethodDecl` an ordinary (non-member) `CallExpr` ever
  // names is a static method reached through its qualified call
  // (`Counter::origin()`) — every non-static method call is a
  // `CXXMemberCallExpr`, intercepted above before reaching this dispatch.
  // Its symbol was mangled at import time via `cxxMethodMangledName`,
  // decoupled from `mlirFuncName`'s per-TU static-storage-class tag.
  const auto *staticMethod = llvm::dyn_cast<clang::CXXMethodDecl>(callee);
  std::string name = staticMethod ? cxxMethodMangledName(staticMethod)
                     : vaClone     ? vaClone->name
                                   : mlirFuncName(callee);
  func::FuncOp target = functions.lookup(name);
  if (!target) {
    if (isSystemHeaderDecl(callee))
      return rejectSystemHeaderUse(loc, "call to", callee->getName());
    return emitError(loc) << "unsupported: call to unimported function '"
                          << name << "'";
  }

  // A method-planned callee (Phase 4) takes the owner receiver plus i64
  // element cursors in place of its pointer arguments.
  if (const clang::VarDecl *ownerBase =
          methodPlans.lookup(callee->getCanonicalDecl()))
    return emitMethodCallSite(call, target, ownerBase, loc);

  // A callee with planned string-cursor parameters (CTS 00204) expands
  // each `&p` cursor argument into (shared region slice, in-out cursor)
  // and stores the advanced cursor back after the call.
  if (const clang::FunctionDecl *definition = callee->getDefinition();
      definition && llvm::any_of(definition->parameters(),
                                 [&](const clang::ParmVarDecl *param) {
                                   return cursorParams.contains(param);
                                 }))
    return emitCursorParamCall(call, target, loc);

  FunctionType targetType = target.getFunctionType();
  unsigned namedArgCount = call->getNumArgs();
  if (vaClone) {
    // A monomorphized call passes every argument — named parameters and
    // this site's extras — as ordinary fixed arguments to the clone.
    if (call->getNumArgs() != targetType.getNumInputs())
      return emitError(loc) << "unsupported: call argument count mismatch";
  } else if (callee->isVariadic()) {
    // A fixed-prototype variadic callee (va_list-free body, checked
    // above) takes only its named parameters; the trailing extras are
    // dropped from the call. A dropped extra is never imported — it must
    // not load a by-value struct or borrow an `&s` operand — so an extra
    // whose evaluation has side effects would silently lose them and is
    // rejected instead.
    if (call->getNumArgs() < targetType.getNumInputs())
      return emitError(loc) << "unsupported: call argument count mismatch";
    namedArgCount = targetType.getNumInputs();
    for (unsigned index = namedArgCount; index < call->getNumArgs(); ++index)
      if (call->getArg(index)->HasSideEffects(astContext()))
        return emitError(loc) << "unsupported: extra argument to a variadic "
                                 "call has side effects";
  } else if (call->getNumArgs() != targetType.getNumInputs()) {
    return emitError(loc) << "unsupported: call argument count mismatch";
  }

  // C leaves the argument evaluation order unspecified; materialize every
  // value argument before any borrow-producing argument so that no load is
  // emitted between a `&mut` borrow and the call consuming it (rustc
  // rejects an intervening use of the borrowed place).
  SmallVector<Value> arguments(namedArgCount, Value());
  struct PendingBorrow {
    unsigned index;
    const clang::Expr *expr;
  };
  SmallVector<PendingBorrow, 4> borrows;
  // Cell-slice arguments backed by globals (CTS-P10): the call nests
  // inside one `emitrust.global_cells` region per distinct global,
  // leftmost argument outermost. Only the named-parameter prefix is
  // examined: dropped variadic extras are never imported.
  struct PendingCellGlobal {
    unsigned index;
    const clang::VarDecl *global;
  };
  SmallVector<PendingCellGlobal, 3> cellGlobals;
  for (unsigned index = 0; index < namedArgCount; ++index) {
    const clang::Expr *argument = call->getArg(index);
    Type input = targetType.getInput(index);
    if (auto refType = llvm::dyn_cast<emitrust::RefType>(input);
        refType && llvm::isa<emitrust::CellSliceType>(refType.getPointee())) {
      // A forwarded cell-slice parameter passes as the same SSA value:
      // shared references are freely duplicable, so permuted recursive
      // forwarding needs no reborrow discipline.
      if (const clang::ParmVarDecl *param = asPointerParamRead(argument)) {
        auto it = symbols.find(param);
        if (it != symbols.end() && it->second.getType() == input) {
          arguments[index] = it->second;
          continue;
        }
      }
      // A directly decayed global array borrows its cell-slice for the
      // extent of the call (Pass A pinned exactly these two shapes).
      if (const clang::VarDecl *global = asDecayedGlobalArrayArg(argument)) {
        cellGlobals.push_back({index, global});
        continue;
      }
      return emitError(loc)
             << "unsupported: argument to a cell-slice parameter must be a "
                "whole global array or a forwarded cell-slice parameter";
    }
    if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(input)) {
      borrows.push_back({index, argument});
      continue;
    }
    // An integer-carrier parameter (CTS-P3) takes a plain i64; the
    // pointer-typed argument must itself be a carrier value (a carrier
    // local/parameter read, a null constant, an integer-to-pointer cast,
    // or a carrier-returning call).
    if (isDataPointer(argument->getType()) &&
        input == builder.getIntegerType(64)) {
      FailureOr<Value> carrier = emitCarrierValue(argument);
      if (failed(carrier))
        return failure();
      arguments[index] = *carrier;
      continue;
    }
    // Positioned against the callee's input: a refined
    // (callsite-inferred, FR-29 / CTS 00209) fn-ptr parameter binds a
    // directly-referenced function at the refined signature.
    FailureOr<Value> value = emitPositionedRValue(input, argument);
    if (failed(value))
      return failure();
    arguments[index] = *value;
  }

  // Borrow-producing arguments: each resolves to a fresh borrow of its
  // region base. Two borrows of the same base would alias mutably in Rust;
  // they are rejected rather than emitted.
  SmallVector<const clang::VarDecl *, 4> borrowRoots;
  for (const PendingBorrow &borrow : borrows) {
    const clang::VarDecl *root = nullptr;
    FailureOr<Value> reference = emitBorrowArgument(
        loc, borrow.expr, targetType.getInput(borrow.index), root);
    if (failed(reference))
      return failure();
    if (root && llvm::is_contained(borrowRoots, root))
      return emitError(loc)
             << "unsupported: aliasing mutable pointer arguments (two "
                "arguments borrow object '"
             << root->getName() << "')";
    if (root)
      borrowRoots.push_back(root);
    arguments[borrow.index] = *reference;
  }

  // Open one global_cells region per distinct global cell-slice argument
  // (argument order, leftmost outermost); the call is created inside the
  // innermost region and a scalar result flows out through a staging
  // variable declared before the outermost region.
  Value cellResultStaging;
  SmallVector<emitrust::GlobalCellsOp, 3> openedCellRegions;
  if (!cellGlobals.empty()) {
    if (targetType.getNumResults() == 1)
      cellResultStaging = createVariablePlace(loc, targetType.getResult(0));
    llvm::SmallDenseMap<const clang::VarDecl *, Value, 4> borrowedCells;
    for (const PendingCellGlobal &entry : cellGlobals) {
      if (Value existing = borrowedCells.lookup(entry.global)) {
        arguments[entry.index] = existing;
        continue;
      }
      const GlobalInfo *global = lookupGlobal(entry.global);
      if (!global)
        return emitError(loc) << "unsupported: global '"
                              << entry.global->getName()
                              << "' is not an importable cell-slice base";
      auto cellsOp = builder.create<emitrust::GlobalCellsOp>(
          loc, globalSymbol(global->symbol));
      Block *body = builder.createBlock(
          &cellsOp.getBody(), cellsOp.getBody().end(),
          TypeRange{targetType.getInput(entry.index)}, {loc});
      arguments[entry.index] = body->getArgument(0);
      borrowedCells[entry.global] = body->getArgument(0);
      openedCellRegions.push_back(cellsOp);
    }
  }

  for (auto [index, value] : llvm::enumerate(arguments))
    if (value.getType() != targetType.getInput(index))
      return emitError(loc) << "unsupported: call argument type mismatch";

  // W2.2: a static method has no receiver to dispatch through, so it never
  // goes through `emitrust.method_call`; but it also is NOT a free
  // function at the top level of the emitted crate — it lives inside
  // `impl Struct { ... }` (placed there by its `method_of` tag) and is
  // therefore only reachable through its qualified path `Struct::name`.
  // The plain-symbol call this generic path would otherwise build (the
  // target's mangled name alone, e.g. "Counter_origin") is unresolvable at
  // that call site once the target is materialized inside the impl block,
  // so the call is instead emitted directly as `emitrust.call_opaque` with
  // the qualified name — reusing the already-mangled MLIR symbol on BOTH
  // sides of the `::` (the RED-pinned scheme), so the qualified string is
  // always self-consistent with whatever the Rust emitter later prints for
  // that same function inside its `impl` block.
  if (staticMethod) {
    llvm::StringRef structName =
        assignedStructNames.lookup(staticMethod->getParent());
    if (structName.empty()) // Defensive; the class was already imported.
      return emitError(loc)
             << "unsupported: static call on an unimported class";
    std::string qualified =
        (llvm::Twine(structName) + "::" + target.getName()).str();
    auto opaqueCall = builder.create<emitrust::CallOpaqueOp>(
        loc, targetType.getResults(), builder.getStringAttr(qualified),
        ArrayAttr(), arguments);
    if (opaqueCall->getNumResults() == 0)
      return Value();
    return opaqueCall->getResult(0);
  }

  auto callOp = builder.create<func::CallOp>(loc, target, arguments);
  // CTS-BR (00216): staged byte-region-global slice arguments store their
  // (possibly mutated) images back immediately after the call — the
  // load-modify-store shape of every staged global access.
  if (!pendingStagedGlobalStores.empty()) {
    SmallVector<std::pair<Value, std::string>, 2> stores =
        std::move(pendingStagedGlobalStores);
    pendingStagedGlobalStores.clear();
    for (auto &[place, symbol] : stores) {
      Value value = loadPlace(loc, place);
      builder.create<emitrust::GlobalStoreOp>(loc, value,
                                              globalSymbol(symbol));
    }
  }
  if (!openedCellRegions.empty()) {
    if (cellResultStaging && callOp->getNumResults() == 1)
      builder.create<emitrust::AssignOp>(loc, cellResultStaging,
                                         callOp->getResult(0));
    for (emitrust::GlobalCellsOp cellsOp : llvm::reverse(openedCellRegions)) {
      builder.create<emitrust::YieldOp>(loc);
      builder.setInsertionPointAfter(cellsOp);
    }
    if (callOp->getNumResults() == 0)
      return Value();
    return loadPlace(loc, cellResultStaging);
  }
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
}

FailureOr<Value>
CImporter::emitCXXMemberCall(const clang::CXXMemberCallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  const clang::CXXMethodDecl *method = call->getMethodDecl();
  if (!method || method->isVirtual())
    return emitError(loc) << "unsupported: virtual or unresolved member call";
  // W2.3: a method declared in namespace `std` (`std::vector<T>`'s /
  // `std::string`'s own inherent methods) never has an imported func — no
  // libstdc++ method is ever imported — so it is intercepted here, before
  // the generic imported-method lookup below would reject it as "call to
  // unimported method".
  if (method->getParent()->isInStdNamespace())
    return emitStlMemberCall(call);
  std::string name = cxxMethodMangledName(method);
  func::FuncOp target = functions.lookup(name);
  if (!target)
    return emitError(loc) << "unsupported: call to unimported method '"
                          << name << "'";
  FunctionType targetType = target.getFunctionType();
  if (call->getNumArgs() + 1 != targetType.getNumInputs())
    return emitError(loc) << "unsupported: call argument count mismatch";

  // The implicit object argument may be wrapped in an implicit
  // qualification-adjustment cast (e.g. adding `const` to bind a non-const
  // object to a const method's implicit `const Counter&` parameter); the
  // place-yielding expression underneath is unaffected by qualifiers.
  FailureOr<Value> receiver = emitLValue(
      call->getImplicitObjectArgument()->IgnoreParenImpCasts());
  if (failed(receiver))
    return failure();
  auto receiverLValueType =
      llvm::dyn_cast<emitrust::LValueType>((*receiver).getType());
  if (!receiverLValueType ||
      !llvm::isa<emitrust::StructType>(receiverLValueType.getValueType()))
    return emitError(loc)
           << "unsupported: member call receiver is not a struct place";

  Value addrOf = builder
                     .create<emitrust::AddrOfOp>(loc, targetType.getInput(0),
                                                 *receiver,
                                                 /*is_mut=*/!method->isConst())
                     .getResult();

  // FR-48: the receiver's own identity, for the same-object aliasing check
  // on reference arguments below. A method call borrows the receiver AND
  // each reference argument at once, so `a.m(a)` would emit two borrows of
  // one object — the C path already rejects the equivalent `f(&a, &a)`
  // ("aliasing mutable pointer arguments"), and the receiver is simply one
  // more borrow to include in that rule.
  const clang::Expr *receiverExpr =
      call->getImplicitObjectArgument()->IgnoreParenImpCasts();
  const clang::VarDecl *receiverRoot = placeExprRoot(receiverExpr);
  bool receiverIsThis = rootsAtCxxThis(receiverExpr);
  bool receiverIsMut = !method->isConst();

  SmallVector<Value> arguments(targetType.getNumInputs(), Value());
  arguments[0] = addrOf;
  // Every borrow the call holds at once, so each new one can be checked
  // against all of them. The receiver occupies slot 0 and is a borrow like
  // any other; `borrowedThis` stands in for the receiver object when the
  // call site names it as `this` (no `VarDecl` can express that).
  SmallVector<std::pair<const clang::VarDecl *, bool>, 4> heldBorrows;
  bool borrowedThis = receiverIsThis;
  bool borrowedThisIsMut = receiverIsThis && receiverIsMut;
  if (receiverRoot)
    heldBorrows.push_back({receiverRoot, receiverIsMut});
  for (auto [index, argExpr] : llvm::enumerate(call->arguments())) {
    Type input = targetType.getInput(index + 1);
    // FR-48: a reference PARAMETER on a method takes exactly the borrow
    // argument a free function's does — same `emitBorrowArgument`, same
    // `emitrust.addr_of`. Before FR-48 no method parameter could ever have
    // a ref/mut_ref type (a reference parameter never mapped), so this
    // loop passed every argument by value; that is why the branch is new
    // here rather than pre-existing.
    if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(input)) {
      // Two borrows of one object are sound only if BOTH are shared. Every
      // borrow the call already holds is checked, not just the receiver:
      // `a.m(a)` and `m(*this)` collide with the RECEIVER, while
      // `a.m(x, x)` collides argument-with-argument, and both would emit
      // Rust that fails borrowck. `emitCall` applies the same rule to a
      // free function's arguments; the only thing this adds is that the
      // receiver counts as one of the borrows.
      bool argIsMut = llvm::isa<emitrust::MutRefType>(input);
      const clang::VarDecl *argRoot = placeExprRoot(argExpr);
      bool argIsThis = rootsAtCxxThis(argExpr);
      bool collides = argIsThis && borrowedThis &&
                      (argIsMut || borrowedThisIsMut);
      if (!collides && argRoot)
        for (auto [heldRoot, heldIsMut] : heldBorrows)
          if (heldRoot == argRoot && (argIsMut || heldIsMut)) {
            collides = true;
            break;
          }
      if (collides)
        return emitError(loc)
               << "unsupported: aliasing mutable reference argument and "
                  "method receiver";
      if (argRoot)
        heldBorrows.push_back({argRoot, argIsMut});
      if (argIsThis) {
        borrowedThis = true;
        borrowedThisIsMut = borrowedThisIsMut || argIsMut;
      }
      const clang::VarDecl *unusedRoot = nullptr;
      FailureOr<Value> reference =
          emitBorrowArgument(loc, argExpr, input, unusedRoot);
      if (failed(reference))
        return failure();
      arguments[index + 1] = *reference;
      continue;
    }
    FailureOr<Value> value = emitRValue(argExpr);
    if (failed(value))
      return failure();
    arguments[index + 1] = *value;
  }
  for (auto [index, value] : llvm::enumerate(arguments))
    if (value.getType() != targetType.getInput(index))
      return emitError(loc) << "unsupported: call argument type mismatch";

  auto callOp = builder.create<func::CallOp>(loc, target, arguments);
  callOp->setAttr(emitrust::kMethodCallAttrName, builder.getUnitAttr());
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
}

FailureOr<Value>
CImporter::emitStlVectorIndexPlace(Value receiver,
                                   emitrust::OpaqueType vectorType,
                                   const clang::Expr *idxExpr, Location loc,
                                   llvm::StringRef opName) {
  llvm::StringRef spelling = vectorType.getValue();
  // Strip the "Vec<" prefix and trailing ">".
  llvm::StringRef inner = spelling.substr(4, spelling.size() - 5);
  Type elementType = parseStlElementType(inner);
  if (!elementType)
    return emitError(loc) << "unsupported: " << opName << " element type";
  FailureOr<Value> index = emitRValue(idxExpr);
  if (failed(index))
    return failure();
  if (!llvm::isa<IntegerType>((*index).getType()))
    return emitError(loc) << "unsupported: " << opName
                          << " index must be an integer";
  return builder
      .create<emitrust::SubscriptOp>(
          loc, emitrust::LValueType::get(elementType), receiver, *index)
      .getResult();
}

FailureOr<Value>
CImporter::emitStlMemberCall(const clang::CXXMemberCallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  const clang::CXXMethodDecl *method = call->getMethodDecl();
  std::string methodName = method->getDeclName().isIdentifier()
                                ? method->getName().str()
                                : std::string();
  // The implicit object argument may be wrapped in an implicit
  // qualification-adjustment cast (const-method binding), mirroring
  // `emitCXXMemberCall`.
  FailureOr<Value> receiver = emitLValue(
      call->getImplicitObjectArgument()->IgnoreParenImpCasts());
  if (failed(receiver))
    return failure();
  auto receiverLValueType =
      llvm::dyn_cast<emitrust::LValueType>((*receiver).getType());
  auto opaque = receiverLValueType ? llvm::dyn_cast<emitrust::OpaqueType>(
                                         receiverLValueType.getValueType())
                                   : emitrust::OpaqueType();
  if (!opaque || !isStlOpaqueType(opaque))
    return emitError(loc)
           << "unsupported: member call receiver is not a recognized STL "
              "type";
  llvm::StringRef typeSpelling = opaque.getValue();
  bool isVector = typeSpelling.starts_with("Vec<");

  // size()/length(): C's declared result type is int/size_t; the real
  // `.len()` returns `usize` (`index`), so the result is cast to whatever
  // the call expression's own static type maps to (mirroring
  // `emitAtoiCall`/`emitFileReadWrite`'s "cast to the C declared type"
  // convention).
  auto emitLenCall = [&]() -> FailureOr<Value> {
    if (call->getNumArgs() != 0)
      return emitError(loc) << "unsupported: " << methodName
                            << " takes no arguments";
    Value len = builder
                    .create<emitrust::MethodCallOp>(
                        loc, TypeRange{builder.getIndexType()}, *receiver,
                        builder.getStringAttr("len"), ValueRange{})
                    .getResult(0);
    FailureOr<Type> resultType = mapType(call->getType(), loc);
    if (failed(resultType))
      return failure();
    auto intType = llvm::dyn_cast<IntegerType>(*resultType);
    if (!intType)
      return emitError(loc) << "unsupported: " << methodName << " result type";
    return builder.create<emitrust::CastOp>(loc, intType, len).getResult();
  };
  auto emitEmptyCall = [&]() -> FailureOr<Value> {
    if (call->getNumArgs() != 0)
      return emitError(loc) << "unsupported: empty takes no arguments";
    return builder
        .create<emitrust::MethodCallOp>(loc, TypeRange{builder.getI1Type()},
                                        *receiver,
                                        builder.getStringAttr("is_empty"),
                                        ValueRange{})
        .getResult(0);
  };

  if (isVector) {
    if (methodName == "push_back") {
      if (call->getNumArgs() != 1)
        return emitError(loc)
               << "unsupported: push_back requires exactly one argument";
      FailureOr<Value> argument = emitRValue(call->getArg(0));
      if (failed(argument))
        return failure();
      builder.create<emitrust::MethodCallOp>(
          loc, TypeRange(), *receiver, builder.getStringAttr("push"),
          ValueRange{*argument});
      return Value();
    }
    if (methodName == "size")
      return emitLenCall();
    if (methodName == "empty")
      return emitEmptyCall();
    if (methodName == "clear") {
      if (call->getNumArgs() != 0)
        return emitError(loc) << "unsupported: clear takes no arguments";
      builder.create<emitrust::MethodCallOp>(loc, TypeRange(), *receiver,
                                             builder.getStringAttr("clear"),
                                             ValueRange{});
      return Value();
    }
    if (methodName == "at") {
      if (call->getNumArgs() != 1)
        return emitError(loc)
               << "unsupported: at requires exactly one argument";
      FailureOr<Value> place = emitStlVectorIndexPlace(
          *receiver, opaque, call->getArg(0), loc, "at");
      if (failed(place))
        return failure();
      return loadPlace(loc, *place);
    }
    return emitError(loc) << "unsupported: std::vector::" << methodName
                          << " is not a recognized STL method";
  }
  // std::string.
  if (methodName == "size" || methodName == "length")
    return emitLenCall();
  if (methodName == "empty")
    return emitEmptyCall();
  if (methodName == "c_str")
    return emitError(loc)
           << "unsupported: std::string::c_str() is only recognized as a "
              "printf '%s' argument";
  return emitError(loc) << "unsupported: std::string::" << methodName
                        << " is not a recognized STL method";
}

FailureOr<Value>
CImporter::emitStlOperatorCall(const clang::CXXOperatorCallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  FailureOr<Value> receiver =
      emitLValue(call->getArg(0)->IgnoreParenImpCasts());
  if (failed(receiver))
    return failure();
  auto receiverLValueType =
      llvm::dyn_cast<emitrust::LValueType>((*receiver).getType());
  auto opaque = receiverLValueType ? llvm::dyn_cast<emitrust::OpaqueType>(
                                         receiverLValueType.getValueType())
                                   : emitrust::OpaqueType();
  if (!opaque || !isStlOpaqueType(opaque))
    return emitError(loc)
           << "unsupported: operator call receiver is not a recognized STL "
              "type";
  bool isVector = opaque.getValue().starts_with("Vec<");

  switch (call->getOperator()) {
  case clang::OO_Subscript: {
    if (!isVector)
      return emitError(loc)
             << "unsupported: std::string::operator[] is not a recognized "
                "STL method (bytes indexing is not supported this wave)";
    if (call->getNumArgs() != 2)
      return emitError(loc)
             << "unsupported: operator[] requires exactly one index "
                "argument";
    FailureOr<Value> place = emitStlVectorIndexPlace(
        *receiver, opaque, call->getArg(1), loc, "operator[]");
    if (failed(place))
      return failure();
    return loadPlace(loc, *place);
  }
  case clang::OO_PlusEqual: {
    if (isVector)
      return emitError(loc)
             << "unsupported: std::vector::operator+= is not a recognized "
                "STL method";
    if (call->getNumArgs() != 2)
      return emitError(loc)
             << "unsupported: operator+= requires exactly one argument";
    const clang::Expr *rhs = call->getArg(1);
    clang::QualType rhsType = rhs->getType().getCanonicalType();
    // `s += "literal"`: `push_str("literal")` — the ordinary string
    // literal is emitted verbatim and needs no borrow (unlike the
    // std::string-operand shape below), mirroring printf `%s`'s literal
    // shape.
    if (const clang::StringLiteral *literal =
            underlyingStringLiteral(rhs->IgnoreParenImpCasts())) {
      if (!literal->isOrdinary())
        return emitError(loc) << "unsupported: non-ordinary string literal "
                                 "in std::string::operator+=";
      FailureOr<Value> text = emitRustStrLiteral(
          loc, literal->getString(), "std::string::operator+=");
      if (failed(text))
        return failure();
      builder.create<emitrust::MethodCallOp>(
          loc, TypeRange(), *receiver, builder.getStringAttr("push_str"),
          ValueRange{*text});
      return Value();
    }
    if (const auto *rhsRecord = rhsType->getAs<clang::RecordType>();
        rhsRecord && rhsRecord->getDecl()->isInStdNamespace()) {
      // `s1 += s2`: `push_str(&s2)` — deref coercion `&String` -> `&str`
      // applies at the argument position (mirroring `c_str()`'s printf
      // borrow and `__emitrust_sprintf`'s staged String borrow).
      FailureOr<Value> rhsPlace = emitLValue(rhs->IgnoreParenImpCasts());
      if (failed(rhsPlace))
        return failure();
      auto rhsLValueType =
          llvm::dyn_cast<emitrust::LValueType>((*rhsPlace).getType());
      if (!rhsLValueType || !isStlOpaqueType(rhsLValueType.getValueType()) ||
          llvm::cast<emitrust::OpaqueType>(rhsLValueType.getValueType())
                  .getValue() != "String")
        return emitError(loc) << "unsupported: operator+= right-hand side "
                                 "is not a recognized std::string";
      Value rhsRef =
          builder
              .create<emitrust::AddrOfOp>(
                  loc, emitrust::RefType::get(rhsLValueType.getValueType()),
                  *rhsPlace, /*is_mut=*/false)
              .getResult();
      builder.create<emitrust::MethodCallOp>(
          loc, TypeRange(), *receiver, builder.getStringAttr("push_str"),
          ValueRange{rhsRef});
      return Value();
    }
    if (rhsType->isIntegerType()) {
      // `s += 'c'`: `push(c as char)`, reusing the printf '%c' char
      // conversion (`wrapCharFormat`) — the same ASCII-only policy.
      FailureOr<Value> rhsValue = emitRValue(rhs);
      if (failed(rhsValue))
        return failure();
      Value character = wrapCharFormat(loc, *rhsValue);
      builder.create<emitrust::MethodCallOp>(loc, TypeRange(), *receiver,
                                             builder.getStringAttr("push"),
                                             ValueRange{character});
      return Value();
    }
    return emitError(loc)
           << "unsupported: std::string::operator+= right-hand side type";
  }
  default:
    return emitError(loc) << "unsupported: this STL operator is not a "
                             "recognized STL method";
  }
}

FailureOr<Value> CImporter::emitMethodCallSite(
    const clang::CallExpr *call, func::FuncOp target,
    const clang::VarDecl *ownerBase, Location loc,
    const clang::VarDecl **outFirstPointerArgBase) {
  if (outFirstPointerArgBase)
    *outFirstPointerArgBase = nullptr;
  FunctionType targetType = target.getFunctionType();
  if (call->getNumArgs() + 1 != targetType.getNumInputs())
    return emitError(loc) << "unsupported: call argument count mismatch";

  // The owner place in the current context: the dereferenced receiver in a
  // sibling method (rendering `(*self).m(...)` after conversion), or the
  // owner struct variable in the owning function.
  Value ownerPlace = currentMethodOwner == ownerBase
                         ? currentReceiverPlace
                         : ownerStructPlaces.lookup(ownerBase);
  if (!ownerPlace) // Defensive; the owner plan covers every call site.
    return emitError(loc) << "unsupported: call to owner method '"
                          << target.getSymName()
                          << "' outside its owner's scope";

  // Every argument is a plain value — pointer arguments lower to their i64
  // element cursors — so the receiver borrow below is the only reference
  // and is materialized last, immediately before the call (one-statement
  // borrow; no load intervenes).
  SmallVector<Value> arguments(targetType.getNumInputs(), Value());
  for (auto [index, argument] : llvm::enumerate(call->arguments())) {
    if (isPointerType(argument->getType()) &&
        !isFunctionPointer(argument->getType())) {
      FailureOr<PtrExprValue> pointer = emitPointerRValue(argument);
      if (failed(pointer))
        return failure();
      // Defensive: the interprocedural plan guarantees the argument points
      // into the owner's region — directly at the owner base in the owning
      // function, or through the current method's own decomposed pointer
      // parameters in a sibling method.
      bool rootedAtOwner =
          pointer->base == ownerBase ||
          (currentMethodOwner == ownerBase &&
           llvm::isa_and_nonnull<clang::ParmVarDecl>(pointer->base));
      if (!rootedAtOwner)
        return emitError(loc)
               << "unsupported: pointer argument does not point into owner "
                  "object '"
               << ownerBase->getName() << "'";
      // Defensive: owner planning excludes nullable regions, so no
      // discriminant should survive to a method-call cursor argument.
      if (pointer->nonNull)
        return emitError(loc) << "unsupported: possibly-null pointer passed "
                                 "as a function argument";
      if (!pointer->cursor) // Defensive; an array base always has a cursor.
        return emitError(loc) << "unsupported: the address of a scalar "
                                 "object cannot index an owner method";
      if (outFirstPointerArgBase && !*outFirstPointerArgBase)
        *outFirstPointerArgBase = pointer->base;
      arguments[index + 1] = pointer->cursor;
      continue;
    }
    FailureOr<Value> value = emitRValue(argument);
    if (failed(value))
      return failure();
    arguments[index + 1] = *value;
  }
  arguments[0] = builder
                     .create<emitrust::AddrOfOp>(loc, targetType.getInput(0),
                                                 ownerPlace, /*is_mut=*/true)
                     .getResult();

  for (auto [index, value] : llvm::enumerate(arguments))
    if (value.getType() != targetType.getInput(index))
      return emitError(loc) << "unsupported: call argument type mismatch";

  auto callOp = builder.create<func::CallOp>(loc, target, arguments);
  callOp->setAttr(emitrust::kMethodCallAttrName, builder.getUnitAttr());
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
}

bool CImporter::involvesDecomposedPointer(const clang::Stmt *stmt) const {
  if (!stmt)
    return false;
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
      if (pointerLocals.contains(var))
        return true;
  for (const clang::Stmt *child : stmt->children())
    if (involvesDecomposedPointer(child))
      return true;
  return false;
}

FailureOr<Value> CImporter::emitCursorParamCall(const clang::CallExpr *call,
                                                func::FuncOp target,
                                                Location loc) {
  const clang::FunctionDecl *definition =
      call->getDirectCallee()->getDefinition();
  FunctionType targetType = target.getFunctionType();
  if (call->getNumArgs() != definition->getNumParams())
    return emitError(loc) << "unsupported: call argument count mismatch";
  // Formal-to-input slot mapping: a cursor parameter owns two inputs
  // (shared region slice, in-out i64 cursor).
  unsigned numParams = definition->getNumParams();
  SmallVector<unsigned, 4> slots(numParams, 0);
  unsigned slot = 0;
  for (unsigned index = 0; index < numParams; ++index) {
    slots[index] = slot;
    slot += cursorParams.contains(definition->getParamDecl(index)) ? 2 : 1;
  }
  if (slot != targetType.getNumInputs())
    return emitError(loc) << "unsupported: call argument count mismatch";

  SmallVector<Value> arguments(targetType.getNumInputs(), Value());
  struct PendingCursor {
    unsigned index;
    const clang::VarDecl *pointer;
  };
  struct PendingBorrow {
    unsigned index;
    const clang::Expr *expr;
  };
  SmallVector<PendingCursor, 2> cursorArgs;
  SmallVector<PendingBorrow, 4> borrows;
  // Value arguments materialize first (C leaves evaluation order
  // unspecified); every borrow-producing argument follows, immediately
  // ahead of the call.
  for (unsigned index = 0; index < numParams; ++index) {
    const clang::Expr *argument = call->getArg(index);
    if (cursorParams.contains(definition->getParamDecl(index))) {
      // The argument must be `&p` over a decomposed pointer local — the
      // caller-side string cursor whose advancement the callee writes.
      const clang::Expr *stripped = stripTrivia(argument);
      while (const auto *cast =
                 llvm::dyn_cast<clang::ImplicitCastExpr>(stripped))
        stripped = stripTrivia(cast->getSubExpr());
      const auto *addrOf = llvm::dyn_cast<clang::UnaryOperator>(stripped);
      const clang::VarDecl *pointer =
          addrOf && addrOf->getOpcode() == clang::UO_AddrOf
              ? asLocalVarRef(addrOf->getSubExpr())
              : nullptr;
      if (!pointer || !pointerLocals.contains(pointer))
        return emitError(loc)
               << "unsupported: a string-cursor argument must be the "
                  "address of a decomposed pointer local";
      cursorArgs.push_back({index, pointer});
      continue;
    }
    Type input = targetType.getInput(slots[index]);
    if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(input)) {
      borrows.push_back({index, argument});
      continue;
    }
    FailureOr<Value> value = emitRValue(argument);
    if (failed(value))
      return failure();
    arguments[slots[index]] = *value;
  }
  for (const PendingBorrow &borrow : borrows) {
    const clang::VarDecl *root = nullptr;
    FailureOr<Value> reference = emitBorrowArgument(
        loc, borrow.expr, targetType.getInput(slots[borrow.index]), root);
    if (failed(reference))
      return failure();
    arguments[slots[borrow.index]] = *reference;
  }
  struct StagedCursor {
    Value tmpPlace;
    Value cell;
  };
  SmallVector<StagedCursor, 2> stagedCursors;
  IntegerType i64Type = builder.getIntegerType(64);
  for (const PendingCursor &cursorArg : cursorArgs) {
    const PointerLocalInfo &info =
        pointerLocals.find(cursorArg.pointer)->second;
    if (!info.cursorCell || info.nonNullCell || info.baseIndexCell ||
        info.member)
      return emitError(loc)
             << "unsupported: a string-cursor argument must walk a single "
                "byte region";
    Value basePlace = info.literalBacking;
    if (!basePlace) {
      auto it = symbols.find(info.base);
      if (it == symbols.end())
        return emitError(loc)
               << "unsupported: pointer target '" << info.base->getName()
               << "' is not an importable place";
      basePlace = it->second;
    }
    auto lvalueType =
        llvm::dyn_cast<emitrust::LValueType>(basePlace.getType());
    Type elementType;
    if (lvalueType) {
      if (auto arrayType =
              llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType()))
        elementType = arrayType.getElementType();
      else if (auto sliceType = llvm::dyn_cast<emitrust::SliceType>(
                   lvalueType.getValueType()))
        elementType = sliceType.getElementType();
    }
    if (elementType != builder.getIntegerType(8))
      return emitError(loc) << "unsupported: a string-cursor argument must "
                               "walk a byte region";
    unsigned slotIndex = slots[cursorArg.index];
    // Shared whole-region slice: the callee only reads bytes; the cursor
    // travels separately, so the slice starts at element zero and both
    // sides speak absolute positions.
    Value zero = createIntConstant(loc, i64Type, 0);
    arguments[slotIndex] =
        builder
            .create<emitrust::SliceOfOp>(loc, targetType.getInput(slotIndex),
                                         basePlace, zero, /*is_mut=*/false)
            .getResult();
    // The in-out cursor stages through a temp the callee writes; the
    // caller's cursor cell takes the advanced value back after the call.
    Value tmpPlace = createVariablePlace(loc, i64Type);
    Value current = loadPlace(loc, info.cursorCell);
    builder.create<emitrust::AssignOp>(loc, tmpPlace, current);
    arguments[slotIndex + 1] =
        builder
            .create<emitrust::AddrOfOp>(loc,
                                        targetType.getInput(slotIndex + 1),
                                        tmpPlace, /*is_mut=*/true)
            .getResult();
    stagedCursors.push_back({tmpPlace, info.cursorCell});
  }
  for (auto [index, value] : llvm::enumerate(arguments))
    if (!value || value.getType() != targetType.getInput(index))
      return emitError(loc) << "unsupported: call argument type mismatch";
  auto callOp = builder.create<func::CallOp>(loc, target, arguments);
  for (const StagedCursor &staged : stagedCursors) {
    Value advanced = loadPlace(loc, staged.tmpPlace);
    builder.create<memref::StoreOp>(loc, advanced, staged.cell);
  }
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
}

FailureOr<Value> CImporter::emitBorrowArgument(Location loc,
                                               const clang::Expr *argument,
                                               Type paramType,
                                               const clang::VarDecl *&root) {
  root = nullptr;
  // Shared byte-slice parameters (`const unsigned char *`, CTS-BR 00216)
  // and `const T&` parameters (FR-48) borrow immutably; everything else
  // borrows mutably.
  Type pointee = borrowPointee(paramType);
  bool isMutParam = llvm::isa<emitrust::MutRefType>(paramType);
  if (!pointee)
    return emitError(loc) << "unsupported reference parameter type";

  // FR-48: the argument bound to a C++ reference parameter. C spells this
  // borrow explicitly (`f(&x)`) and C++ does not (`f(x)`) — the argument
  // is a bare lvalue of the referent's type with no address-of node — so
  // the ONE thing that has to be reconstructed is the borrow itself. The
  // discriminator is the argument's own type: it is a non-pointer lvalue
  // exactly when the parameter it feeds is a reference, since every other
  // path into this function passes a pointer-typed expression.
  //
  // Everything downstream is then shared with the C path unchanged: the
  // place comes from `emitLValue` (so a plain variable, a member, and an
  // element all work, and a global correctly hits
  // `rejectGlobalPointerArgument` rather than silently borrowing a staged
  // copy), the borrow is the same `emitrust.addr_of`, and `root` is set so
  // `emitCall`'s same-base aliasing rejection covers `f(x, x)` — the C++
  // spelling of the `f(&x, &x)` it already rejects.
  if (!argument->getType()->isPointerType() && argument->isLValue()) {
    const clang::Expr *placeExpr = stripLValueNoOp(argument);
    // FR-48: `f(a + b)` and `f(7)` on a `const T&` parameter. C++ lets a
    // PRVALUE bind to a const reference by materializing a temporary that
    // lives for the full-expression, which clang spells as a
    // `MaterializeTemporaryExpr` — an lvalue with no underlying object for
    // `emitLValue` to find. Staging the value into a fresh local place and
    // borrowing THAT reproduces the C++ lifetime exactly (`let t = ...;
    // f(&t)`), and it is sound precisely because the borrow is shared: a
    // temporary has no other name, so nothing can observe it, and there is
    // no caller-visible write to lose.
    //
    // A MUTABLE reference never reaches here — C++ itself forbids binding a
    // prvalue to `T&` — so the `isMutParam` guard is a belt-and-braces
    // check against ever borrowing a temporary mutably, which would
    // silently discard the callee's writes.
    if (const auto *temporary =
            llvm::dyn_cast<clang::MaterializeTemporaryExpr>(placeExpr)) {
      if (isMutParam)
        return emitError(loc)
               << "unsupported: mutable reference bound to a temporary";
      FailureOr<Value> value = emitRValue(temporary->getSubExpr());
      if (failed(value))
        return failure();
      if ((*value).getType() != pointee)
        return emitError(loc) << "unsupported: argument type does not match "
                                 "the reference parameter";
      Value staged = createVariablePlace(loc, pointee);
      builder.create<emitrust::AssignOp>(loc, staged, *value);
      // `root` stays null on purpose: a temporary is a fresh object that
      // aliases nothing, so it can never collide in the same-base check.
      return builder
          .create<emitrust::AddrOfOp>(loc, paramType, staged, isMutParam)
          .getResult();
    }
    const clang::VarDecl *localRoot = placeExprRoot(placeExpr);
    if (localRoot && !localRoot->hasLocalStorage())
      return rejectGlobalPointerArgument(loc, localRoot);
    FailureOr<Value> place = emitLValue(placeExpr);
    if (failed(place))
      return failure();
    auto lvalueType =
        llvm::dyn_cast<emitrust::LValueType>((*place).getType());
    if (!lvalueType || lvalueType.getValueType() != pointee)
      return emitError(loc) << "unsupported: argument type does not match "
                               "the reference parameter";
    root = localRoot;
    return builder
        .create<emitrust::AddrOfOp>(loc, paramType, *place, isMutParam)
        .getResult();
  }

  if (auto sliceType = llvm::dyn_cast<emitrust::SliceType>(pointee)) {
    // A string-literal argument (`f("abc")`) materializes a fresh mutable
    // backing byte array (bytes plus the terminating NUL) at the call
    // site and passes a whole-array slice of it. Per-call copies are
    // unobservable in defined C programs: identical literals may share
    // storage in C, but writing through a pointer to a string literal is
    // undefined behavior, so no defined program can distinguish a copy
    // from C's shared read-only storage.
    const clang::Expr *strippedArg = stripTrivia(argument);
    while (const auto *noop =
               llvm::dyn_cast<clang::ImplicitCastExpr>(strippedArg)) {
      if (noop->getCastKind() != clang::CK_NoOp)
        break;
      strippedArg = stripTrivia(noop->getSubExpr());
    }
    if (const auto *decay =
            llvm::dyn_cast<clang::ImplicitCastExpr>(strippedArg))
      if (decay->getCastKind() == clang::CK_ArrayToPointerDecay)
        if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(
                stripTrivia(decay->getSubExpr()))) {
          if (sliceType.getElementType() != builder.getIntegerType(8))
            return emitError(loc)
                   << "unsupported: argument element type does not "
                      "match the slice parameter";
          FailureOr<Value> backing =
              createLiteralBacking(literal, loc, /*isConst=*/false);
          if (failed(backing))
            return failure();
          Value zero =
              createIntConstant(loc, builder.getIntegerType(64), 0);
          return builder
              .create<emitrust::SliceOfOp>(loc, paramType, *backing, zero,
                                           /*is_mut=*/isMutParam)
              .getResult();
        }
    // Slice parameter: reslice the argument's region base from its cursor.
    // A decayed array decomposes to cursor 0, `&arr[i]` to cursor i, a
    // walking pointer to its current cursor, and a slice parameter's own
    // read composes through the deref'd base place.
    FailureOr<PtrExprValue> pointer = emitPointerRValue(argument);
    if (failed(pointer))
      return failure();
    // A `const char *` variable that points into a string literal resolves
    // to a base-less region whose storage is the literal's read-only backing
    // byte array (`literalBacking`, cursor always present). It has no VarDecl
    // base, so it must be handled here before any of the base-keyed paths
    // below dereference `pointer->base` (the historical null-base SIGSEGV).
    // The reslice mirrors the direct-string-literal argument path above:
    // a mutable slice parameter rematerializes a FRESH mutable backing so it
    // never aliases the shared read-only literal storage (writing through a
    // pointer to a string literal is undefined behavior, so the per-call copy
    // is unobservable to any defined program), while a shared byte-slice
    // parameter borrows the const backing read-only.
    if (pointer->literalBacking) {
      if (sliceType.getElementType() != builder.getIntegerType(8))
        return emitError(loc)
               << "unsupported: argument element type does not "
                  "match the slice parameter";
      Value backing = pointer->literalBacking;
      if (isMutParam) {
        auto sourceVar =
            pointer->literalBacking.getDefiningOp<emitrust::VariableOp>();
        if (!sourceVar)
          return emitError(loc)
                 << "unsupported: string-literal argument to a mutable slice "
                    "parameter has no backing to copy";
        backing = builder
                      .create<emitrust::VariableOp>(
                          loc, pointer->literalBacking.getType(),
                          sourceVar.getInitAttr(), /*isConst=*/false)
                      .getResult();
      }
      Value cursor =
          pointer->cursor
              ? pointer->cursor
              : createIntConstant(loc, builder.getIntegerType(64), 0);
      return builder
          .create<emitrust::SliceOfOp>(loc, paramType, backing, cursor,
                                       /*is_mut=*/isMutParam)
          .getResult();
    }
    root = pointer->base;
    // A multi-base pointer has no single region base to reslice; the
    // callee would need the enum-of-bases discriminant, which a slice
    // parameter cannot carry (CTS-P7 scope).
    if (pointer->baseIndex)
      return emitError(loc) << "unsupported: passing a pointer bound to "
                               "multiple objects to a function";
    // Parameters have no null representation; passing a possibly-null
    // pointer would erase its discriminant (and the callee may be a
    // defined C program that null-checks it).
    if (pointer->nonNull)
      return emitError(loc) << "unsupported: possibly-null pointer passed "
                               "as a function argument";
    // A global region base would pass a borrow of the staged local copy,
    // not of the global itself: a callee that also touches the global
    // would see (or lose) the wrong values, so the shape is rejected
    // (with the cell-slice boundary wording when Pass A pinned one; the
    // all-global class itself lowers to a cell-slice and never gets
    // here). EXCEPT a byte-region aggregate global (CTS-BR, 00216),
    // which rides the ordinary staged-copy model: the borrow is of a
    // whole staged byte image, and a mutable parameter stores the image
    // back immediately after the call.
    if (pointer->base && !pointer->base->hasLocalStorage()) {
      if (!isByteRegionAggregate(pointer->base->getType()))
        return rejectGlobalPointerArgument(loc, pointer->base);
      FailureOr<std::pair<Value, std::string>> staged =
          stageGlobalCopy(loc, pointer->base);
      if (failed(staged))
        return failure();
      if (isMutParam)
        pendingStagedGlobalStores.push_back(*staged);
      Value cursor =
          pointer->cursor
              ? pointer->cursor
              : createIntConstant(loc, builder.getIntegerType(64), 0);
      return builder
          .create<emitrust::SliceOfOp>(loc, paramType, staged->first,
                                       cursor, isMutParam)
          .getResult();
    }
    if (!pointer->cursor)
      return emitError(loc) << "unsupported: the address of a scalar object "
                               "cannot be passed as a slice parameter";
    auto it = symbols.find(pointer->base);
    if (it == symbols.end())
      return emitError(loc) << "unsupported: pointer target '"
                            << pointer->base->getName()
                            << "' is not an importable place";
    Value basePlace = it->second;
    auto lvalueType =
        llvm::dyn_cast<emitrust::LValueType>(basePlace.getType());
    Type elementType;
    if (lvalueType) {
      if (auto arrayType =
              llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType()))
        elementType = arrayType.getElementType();
      else if (auto baseSlice = llvm::dyn_cast<emitrust::SliceType>(
                   lvalueType.getValueType()))
        elementType = baseSlice.getElementType();
    }
    if (!elementType)
      return emitError(loc) << "unsupported pointer target place";
    if (elementType != sliceType.getElementType())
      return emitError(loc) << "unsupported: argument element type does not "
                               "match the slice parameter";
    return builder
        .create<emitrust::SliceOfOp>(loc, paramType, basePlace,
                                     pointer->cursor, /*is_mut=*/isMutParam)
        .getResult();
  }

  // C's enum/underlying-type pointer compatibility (C99 6.7.2.2p4): the
  // address of an enum object passed as a pointer to the enum's underlying
  // integer type arrives as an implicit BitCast around the address-of.
  // The borrow resolves to the enum place's raw-representation lvalue
  // (`emitrust.enum_raw`, rendered `.0` on the open-enum tuple struct),
  // whose value type is the enum's storage integer. Any other pointer
  // BitCast keeps its located rejection through emitPointerRValue below.
  if (const auto *bitcast =
          llvm::dyn_cast<clang::ImplicitCastExpr>(stripTrivia(argument));
      bitcast && bitcast->getCastKind() == clang::CK_BitCast) {
    const auto *innerAddrOf = llvm::dyn_cast<clang::UnaryOperator>(
        stripTrivia(bitcast->getSubExpr()));
    const auto *ref =
        innerAddrOf && innerAddrOf->getOpcode() == clang::UO_AddrOf
            ? llvm::dyn_cast<clang::DeclRefExpr>(
                  stripTrivia(innerAddrOf->getSubExpr()))
            : nullptr;
    const auto *var =
        ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
    const clang::EnumDecl *enumDecl =
        var ? namedEnumDeclOf(var->getType()) : nullptr;
    if (enumDecl && bitcast->getType()->isPointerType() &&
        astContext().hasSameUnqualifiedType(
            bitcast->getType()->getPointeeType(),
            enumDecl->getIntegerType())) {
      auto it = symbols.find(var);
      if (it == symbols.end() ||
          !llvm::isa<emitrust::LValueType>(it->second.getType()))
        return emitError(loc) << "unsupported: enum object '"
                              << var->getName()
                              << "' is not an addressable place";
      FailureOr<Type> rawType = mapType(enumDecl->getIntegerType(), loc);
      if (failed(rawType))
        return failure();
      if (*rawType != pointee)
        return emitError(loc) << "unsupported: argument type does not match "
                                 "the pointer parameter";
      root = var;
      Value rawPlace = builder
                           .create<emitrust::EnumRawOp>(
                               loc, emitrust::LValueType::get(*rawType),
                               it->second)
                           .getResult();
      return builder
          .create<emitrust::AddrOfOp>(loc, paramType, rawPlace, isMutParam)
          .getResult();
    }
  }

  // Scalar-reference parameter. Arguments involving a decomposed pointer
  // (or any non-address-of pointer expression, e.g. a decayed array) borrow
  // the designated element through the decomposition; plain address-of
  // arguments (`&x`, `&s.f`, `&arr[i]`) keep the historical
  // emitLValue+addr_of path.
  const clang::Expr *stripped = stripTrivia(argument);
  const auto *addrOf = llvm::dyn_cast<clang::UnaryOperator>(stripped);
  bool isAddressOf = addrOf && addrOf->getOpcode() == clang::UO_AddrOf;
  if (!isAddressOf || involvesDecomposedPointer(argument)) {
    FailureOr<PtrExprValue> pointer = emitPointerRValue(argument);
    if (failed(pointer))
      return failure();
    root = pointer->base;
    // Parameters have no null representation; see the slice path above.
    if (pointer->nonNull)
      return emitError(loc) << "unsupported: possibly-null pointer passed "
                               "as a function argument";
    // A global region base would pass a borrow of the staged local copy
    // (writes through it would be lost); mirror the historical
    // address-of-a-global rejection (with the cell-slice boundary
    // wording when Pass A pinned one).
    if (pointer->base && !pointer->base->hasLocalStorage())
      return rejectGlobalPointerArgument(loc, pointer->base);
    FailureOr<Value> place = emitPointerPlace(loc, *pointer, pointee);
    if (failed(place))
      return failure();
    auto lvalueType = llvm::cast<emitrust::LValueType>((*place).getType());
    if (lvalueType.getValueType() != pointee)
      return emitError(loc) << "unsupported: argument type does not match "
                               "the pointer parameter";
    // FR-55: the borrow's mutability is the PARAMETER's, not a constant.
    // A `const unsigned char *` scalar-reference parameter maps to `&u8`
    // (see `mapParamType`), and borrowing its argument mutably would both
    // fail `AddrOfOp`'s own marker/type verifier and, where the argument
    // designates an element of a shared byte slice, emit Rust that cannot
    // compile.
    return builder
        .create<emitrust::AddrOfOp>(loc, paramType, *place, isMutParam)
        .getResult();
  }
  root = addressArgumentRoot(argument);
  // FR-55: `&x` has no shared spelling in C — the address-of rvalue always
  // yields `!emitrust.mut_ref` — so a SHARED scalar-reference parameter
  // cannot go through `emitRValue`. Build the borrow at the parameter's own
  // mutability from the same place instead; the global rejection and the
  // pointee type check are the ones the rvalue path would have applied.
  if (!isMutParam) {
    if (rootsAtGlobal(addrOf->getSubExpr()))
      return emitError(loc)
             << "unsupported: taking the address of a global variable";
    FailureOr<Value> place = emitLValue(addrOf->getSubExpr());
    if (failed(place))
      return failure();
    auto lvalueType = llvm::dyn_cast<emitrust::LValueType>((*place).getType());
    if (!lvalueType || lvalueType.getValueType() != pointee)
      return emitError(loc) << "unsupported: argument type does not match "
                               "the pointer parameter";
    return builder
        .create<emitrust::AddrOfOp>(loc, paramType, *place, /*is_mut=*/false)
        .getResult();
  }
  FailureOr<Value> value = emitRValue(argument);
  if (failed(value))
    return failure();
  return *value;
}


FailureOr<Value> CImporter::emitIndirectCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  const clang::Expr *calleeExpr = call->getCallee()->IgnoreParens();
  // `(*fp)(...)`: the dereference of a function pointer designates the
  // function, which immediately decays back to the pointer value, so the
  // decay/deref pair cancels out and the pointer itself is evaluated.
  if (const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(calleeExpr))
    if (decay->getCastKind() == clang::CK_FunctionToPointerDecay) {
      const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(
          decay->getSubExpr()->IgnoreParens());
      if (deref && deref->getOpcode() == clang::UO_Deref &&
          isFunctionPointer(deref->getSubExpr()->getType()))
        calleeExpr = deref->getSubExpr()->IgnoreParens();
    }
  if (!isFunctionPointer(calleeExpr->getType()))
    return emitError(loc) << "unsupported: indirect function call";

  // A call with arguments through a prototype-less K&R pointer
  // (`int (*f)()`): a callee traceable to a callsite-inferred decl
  // (FR-29, CTS 00209) proceeds on the ordinary typed path below — the
  // decl's refined fn_ptr value type carries the inferred signature the
  // arguments are checked against. Any other callee (member, array
  // element, call result, uninferred decl) has no signature to check its
  // arguments against; the zero-argument form is the only one that can
  // be verified.
  const clang::Type *pointee = calleeExpr->getType()
                                   .getCanonicalType()
                                   ->getPointeeType()
                                   .getTypePtr();
  if (llvm::isa<clang::FunctionNoProtoType>(pointee) &&
      call->getNumArgs() > 0) {
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
        calleeExpr->IgnoreParenImpCasts());
    const auto *var =
        ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
    if (!var || !inferredFnPtrSigs.contains(var))
      return emitError(loc)
             << "unsupported: call with arguments through a function "
                "pointer without a prototype";
  }

  // A call through a devirtualized alias (CTS-S, 00189) lowers as a
  // DIRECT call to the target: no fn_ptr value and no call_indirect.
  const clang::VarDecl *aliasVar = nullptr;
  if (const clang::FunctionDecl *target =
          devirtualizedCallee(call, &aliasVar))
    return emitDevirtualizedCall(call, aliasVar, target, loc);

  // A cast-call through an admitted local `void *` fn-ptr holder
  // (CTS-F, 00210) peels the cast entirely (clang already discarded any
  // attributes inside the spelled type): the holder's place carries the
  // target-signature fn_ptr value, so the callee is a plain load and no
  // cast op reaches the IR.
  Value holderValue;
  if (const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(calleeExpr)) {
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
        cast->getSubExpr()->IgnoreParenImpCasts());
    const auto *var =
        ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
    if (var && voidFnPtrHolders.contains(var))
      holderValue = loadPlace(loc, symbols.lookup(var));
  }
  FailureOr<Value> fnValue =
      holderValue ? FailureOr<Value>(holderValue) : emitRValue(calleeExpr);
  if (failed(fnValue))
    return failure();
  auto fnPtrType = llvm::dyn_cast<emitrust::FnPtrType>((*fnValue).getType());
  if (!fnPtrType)
    return emitError(loc) << "unsupported: indirect function call";

  // Argument checking mirrors the direct-call path, each argument
  // positioned against the fn_ptr's input (see emitPositionedRValue).
  ArrayRef<Type> inputs = fnPtrType.getInputs();
  SmallVector<Value> arguments;
  for (auto [index, argument] : llvm::enumerate(call->arguments())) {
    FailureOr<Value> value = emitPositionedRValue(
        index < inputs.size() ? inputs[index] : Type(), argument);
    if (failed(value))
      return failure();
    arguments.push_back(*value);
  }
  if (arguments.size() != inputs.size())
    return emitError(loc) << "unsupported: call argument count mismatch";
  for (auto [index, value] : llvm::enumerate(arguments))
    if (value.getType() != inputs[index])
      return emitError(loc) << "unsupported: call argument type mismatch";

  auto callOp = builder.create<emitrust::CallIndirectOp>(
      loc, fnPtrType.getResults(), *fnValue, arguments);
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
}

const clang::FunctionDecl *
CImporter::devirtualizedCallee(const clang::CallExpr *call,
                               const clang::VarDecl **aliasVar) const {
  const clang::Expr *calleeExpr = call->getCallee()->IgnoreParens();
  // `(*fp)(...)`: the decay/deref pair cancels out (see emitIndirectCall).
  if (const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(calleeExpr))
    if (decay->getCastKind() == clang::CK_FunctionToPointerDecay) {
      const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(
          decay->getSubExpr()->IgnoreParens());
      if (deref && deref->getOpcode() == clang::UO_Deref &&
          isFunctionPointer(deref->getSubExpr()->getType()))
        calleeExpr = deref->getSubExpr()->IgnoreParens();
    }
  const auto *ref =
      llvm::dyn_cast<clang::DeclRefExpr>(calleeExpr->IgnoreParenImpCasts());
  if (!ref)
    return nullptr;
  const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
  if (!var)
    return nullptr;
  const clang::FunctionDecl *target = fnPtrAliases.lookup(
      llvm::cast<clang::VarDecl>(var->getCanonicalDecl()));
  if (target && aliasVar)
    *aliasVar = var;
  return target;
}

FailureOr<Value>
CImporter::emitDevirtualizedCall(const clang::CallExpr *call,
                                 const clang::VarDecl *var,
                                 const clang::FunctionDecl *target,
                                 Location loc) {
  // A hosted-variadic alias is statement-position only (the printf
  // machinery has no result); reaching this value path is a rejection.
  if (target->isVariadic())
    return emitError(loc) << "unsupported: " << target->getName()
                          << " return value must be unused";
  FailureOr<Type> mapped = mapType(var->getType(), loc);
  if (failed(mapped))
    return failure();
  auto fnPtrType = llvm::dyn_cast<emitrust::FnPtrType>(*mapped);
  if (!fnPtrType)
    return emitError(loc) << "unsupported function pointer type";
  // The same import + signature check a `Some(target)` constant runs.
  FailureOr<std::string> name =
      resolveFunctionPointerDecl(target, fnPtrType, loc);
  if (failed(name))
    return failure();
  func::FuncOp targetOp = functions.lookup(*name);

  // Argument checking mirrors the indirect-call path.
  SmallVector<Value> arguments;
  for (const clang::Expr *argument : call->arguments()) {
    FailureOr<Value> value = emitRValue(argument);
    if (failed(value))
      return failure();
    arguments.push_back(*value);
  }
  ArrayRef<Type> inputs = fnPtrType.getInputs();
  if (arguments.size() != inputs.size())
    return emitError(loc) << "unsupported: call argument count mismatch";
  for (auto [index, value] : llvm::enumerate(arguments))
    if (value.getType() != inputs[index])
      return emitError(loc) << "unsupported: call argument type mismatch";

  auto callOp = builder.create<func::CallOp>(loc, targetOp, arguments);
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
}

FailureOr<std::string>
CImporter::resolveFunctionPointerTarget(const clang::Expr *expr,
                                        emitrust::FnPtrType fnPtrType,
                                        Location loc) {
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(expr));
  const auto *callee =
      ref ? llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()) : nullptr;
  if (!callee)
    return emitError(loc) << "unsupported: function pointer target is not a "
                             "direct function reference";
  return resolveFunctionPointerDecl(callee, fnPtrType, loc);
}

FailureOr<std::string>
CImporter::resolveFunctionPointerDecl(const clang::FunctionDecl *callee,
                                      emitrust::FnPtrType fnPtrType,
                                      Location loc) {
  // Variadic declarations (printf) are never imported, and a Rust `fn`
  // item cannot be variadic either.
  if (callee->isVariadic())
    return emitError(loc)
           << "unsupported: taking the address of a variadic function";
  std::string name = mlirFuncName(callee);
  func::FuncOp target = functions.lookup(name);
  if (!target) {
    if (isSystemHeaderDecl(callee))
      return rejectSystemHeaderUse(loc, "taking the address of",
                                   callee->getName());
    return emitError(loc)
           << "unsupported: taking the address of unimported function '"
           << name << "'";
  }
  // The imported function's MLIR signature must equal the fn_ptr's
  // component types exactly. This rejects prototype mismatches (including
  // a prototype-less `int (*)()` pointer bound to a function with
  // parameters) and functions whose data-pointer parameters import as
  // references, which no fn_ptr can carry.
  FunctionType targetType = target.getFunctionType();
  if (targetType.getInputs() != fnPtrType.getInputs() ||
      targetType.getResults() != fnPtrType.getResults())
    return emitError(loc)
           << "unsupported: function '" << name
           << "' does not match the function pointer signature";
  // FR-52: the emitted `Some(<name>)` constant spells the item out directly,
  // so this name can never be routed through the external-requirement trait.
  // Recorded here — the one place a function address resolves — rather than
  // recovered later by scanning opaque constant text.
  fnPointerTargetSymbols.insert(name);
  return name;
}

FailureOr<Value>
CImporter::emitFunctionPointerConstant(const clang::Expr *fnExpr,
                                       clang::QualType pointerType,
                                       Location loc) {
  FailureOr<Type> mapped = mapType(pointerType, loc);
  if (failed(mapped))
    return failure();
  auto fnPtrType = llvm::dyn_cast<emitrust::FnPtrType>(*mapped);
  if (!fnPtrType)
    return emitError(loc) << "unsupported function pointer type";
  FailureOr<std::string> name =
      resolveFunctionPointerTarget(fnExpr, fnPtrType, loc);
  if (failed(name))
    return failure();
  auto some = emitrust::OpaqueAttr::get(
      builder.getContext(), (llvm::Twine("Some(") + *name + ")").str());
  return builder.create<emitrust::ConstantOp>(loc, fnPtrType, some)
      .getResult();
}

FailureOr<Value>
CImporter::emitFunctionPointerConstant(const clang::FunctionDecl *target,
                                       emitrust::FnPtrType fnPtrType,
                                       Location loc) {
  FailureOr<std::string> name =
      resolveFunctionPointerDecl(target, fnPtrType, loc);
  if (failed(name))
    return failure();
  auto some = emitrust::OpaqueAttr::get(
      builder.getContext(), (llvm::Twine("Some(") + *name + ")").str());
  return builder.create<emitrust::ConstantOp>(loc, fnPtrType, some)
      .getResult();
}

FailureOr<Value> CImporter::emitPositionedRValue(Type expected,
                                                 const clang::Expr *expr) {
  auto fnPtrType = llvm::dyn_cast_if_present<emitrust::FnPtrType>(expected);
  if (!fnPtrType)
    return emitRValue(expr);
  clang::QualType type = expr->getType().getCanonicalType();
  if (!isFunctionPointer(type) ||
      !llvm::isa<clang::FunctionNoProtoType>(
          type->getPointeeType().getTypePtr()))
    return emitRValue(expr);
  // A prototype-less source binding into a known fn_ptr position: a
  // direct function reference resolves against the position's signature
  // (the refined type when the destination was callsite-inferred, FR-29 /
  // CTS 00209; the identical mapping otherwise), and a null constant is
  // the position's `None`. Anything else — a loaded fn-ptr VALUE keeps
  // its own (possibly unrefined) type, surfacing any mismatch at the
  // consuming check.
  Location loc = translateLoc(expr->getBeginLoc());
  if (const clang::Expr *fnExpr = returnedFunctionExpr(expr)) {
    const auto *target = llvm::cast<clang::FunctionDecl>(
        llvm::cast<clang::DeclRefExpr>(fnExpr)->getDecl());
    return emitFunctionPointerConstant(target, fnPtrType, loc);
  }
  if (expr->isNullPointerConstant(astContext(),
                                  clang::Expr::NPC_ValueDependentIsNull))
    return createFnPtrNone(loc, fnPtrType);
  return emitRValue(expr);
}

Value CImporter::createFnPtrNone(Location loc, Type fnPtrType) {
  auto none = emitrust::OpaqueAttr::get(builder.getContext(), "None");
  return builder.create<emitrust::ConstantOp>(loc, fnPtrType, none)
      .getResult();
}

FailureOr<Value> CImporter::emitEnumConstant(
    const clang::EnumConstantDecl *enumerator, Location loc) {
  const auto *parent =
      llvm::cast<clang::EnumDecl>(enumerator->getDeclContext());
  const clang::EnumDecl *definition = parent->getDefinition();
  if (!definition)
    return emitError(loc) << "unsupported: enumerator of an incomplete enum";
  if (definition->getName().empty()) {
    // Anonymous enums have no Rust counterpart; the enumerator is a plain
    // `int` constant.
    const llvm::APSInt &initValue = enumerator->getInitVal();
    if (!initValue.isRepresentableByInt64() ||
        initValue.getExtValue() < INT32_MIN ||
        initValue.getExtValue() > INT32_MAX)
      return emitError(loc)
             << "unsupported: enumerator value does not fit in i32";
    return createIntConstant(loc, builder.getI32Type(),
                             initValue.getExtValue());
  }
  // Importing the definition validates the enumerator spellings and values
  // even when the enum type itself is never named.
  if (failed(importEnum(definition, loc)))
    return failure();
  std::string path =
      (llvm::Twine(definition->getName()) + "::" + enumerator->getName())
          .str();
  auto type =
      emitrust::EnumType::get(builder.getContext(), definition->getName());
  auto value = emitrust::OpaqueAttr::get(builder.getContext(), path);
  return builder.create<emitrust::ConstantOp>(loc, type, value).getResult();
}

FailureOr<Value> CImporter::emitEnumOperand(const EnumOperand &operand,
                                            Location loc) {
  if (operand.constant)
    return emitEnumConstant(operand.constant, loc);
  return emitRValue(operand.value);
}

Value CImporter::castEnumToI32(Location loc, Value value) {
  return builder.create<emitrust::CastOp>(loc, builder.getI32Type(), value)
      .getResult();
}

bool CImporter::isDecomposedPointerExpr(const clang::Expr *expr) const {
  const clang::Expr *e = stripTrivia(expr);
  // W2.2: `this` is never a decomposed C pointer — it is the method's own
  // reference-typed receiver, handled by the plain `emitRValue`-then-deref
  // path exactly like a pointer parameter's reference SSA value (the
  // DeclRefExpr case immediately below).
  if (llvm::isa<clang::CXXThisExpr>(e))
    return false;
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    if (cast->getCastKind() == clang::CK_LValueToRValue)
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
              stripTrivia(cast->getSubExpr()))) {
        auto it = symbols.find(ref->getDecl());
        if (it != symbols.end() &&
            llvm::isa<emitrust::MutRefType, emitrust::RefType>(
                it->second.getType()))
          return false; // Pointer parameter: existing reference path.
      }
  // Every other pointer rvalue shape belongs to the decomposition, which
  // rejects the unsupported ones with located diagnostics.
  return true;
}

bool CImporter::isNullPointerConstantExpr(const clang::Expr *expr) const {
  if (expr->isNullPointerConstant(astContext(),
                                  clang::Expr::NPC_NeverValueDependent) !=
      clang::Expr::NPCK_NotNull)
    return true;
  // `(const void *) 0`: only the UNQUALIFIED `void *` cast is a formal
  // null pointer constant (C11 6.3.2.3p3), but the qualified cast still
  // yields the null pointer value; clang models both as CK_NullToPointer.
  const auto *cast = llvm::dyn_cast<clang::CastExpr>(stripTrivia(expr));
  return cast && cast->getCastKind() == clang::CK_NullToPointer;
}

bool CImporter::isStaticallyNullPointerExpr(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  while (const clang::Expr *sub = peelPointerCast(astContext(), e))
    e = stripTrivia(sub);
  const clang::VarDecl *var = asLoadedLocalVarRef(e);
  if (!var || !pointerRegions.tracks(var))
    return false;
  return isStaticallyNullRegion(pointerRegions.regionOf(var));
}


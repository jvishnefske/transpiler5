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
// C99-43 C3: admitted argv read lowering
//===----------------------------------------------------------------------===//

const clang::Expr *
CImporter::matchArgvWholeSubscript(const clang::Expr *expr) const {
  // Only an admitted `main` binds the table; without it argv never reaches
  // here (its uses were rejected at the signature), so nothing matches.
  if (!mainArgvTableValue)
    return nullptr;
  const clang::Expr *e = expr->IgnoreParenImpCasts();
  const auto *sub = llvm::dyn_cast<clang::ArraySubscriptExpr>(e);
  if (!sub)
    return nullptr;
  const auto *ref =
      llvm::dyn_cast<clang::DeclRefExpr>(sub->getBase()->IgnoreParenImpCasts());
  if (!ref || ref->getDecl() != mainArgvAdmittedParam)
    return nullptr;
  return sub->getIdx();
}

const clang::ArraySubscriptExpr *
CImporter::matchArgvByteRead(const clang::Expr *expr) const {
  if (!mainArgvTableValue)
    return nullptr;
  const clang::Expr *e = expr->IgnoreParenImpCasts();
  const auto *outer = llvm::dyn_cast<clang::ArraySubscriptExpr>(e);
  if (!outer)
    return nullptr;
  // The outer base must itself be a whole-value `argv[i]` borrow.
  if (!matchArgvWholeSubscript(outer->getBase()))
    return nullptr;
  return outer;
}

FailureOr<Value> CImporter::emitArgvArgSlice(Location loc,
                                             const clang::Expr *indexExpr) {
  FailureOr<Value> index = emitRValue(indexExpr);
  if (failed(index))
    return failure();
  auto sliceRefType = emitrust::RefType::get(
      emitrust::SliceType::get(builder.getIntegerType(8)));
  return builder
      .create<emitrust::ArgvArgOp>(loc, sliceRefType, mainArgvTableValue,
                                   *index)
      .getResult();
}

FailureOr<Value>
CImporter::emitArgvByteLValue(const clang::ArraySubscriptExpr *subscript,
                             Location loc) {
  // The byte place of `argv[i][j]`: borrow argument i's slice, deref to the
  // slice place, subscript byte j — reusing the ordinary slice element read.
  const clang::Expr *innerIndex =
      matchArgvWholeSubscript(subscript->getBase());
  if (!innerIndex)
    return emitError(loc)
           << "unsupported: use of main's argv parameter (command-line "
              "argument values are not modeled)";
  FailureOr<Value> slice = emitArgvArgSlice(loc, innerIndex);
  if (failed(slice))
    return failure();
  auto i8Type = builder.getIntegerType(8);
  Value place =
      builder
          .create<emitrust::DerefOp>(
              loc, emitrust::LValueType::get(emitrust::SliceType::get(i8Type)),
              *slice)
          .getResult();
  FailureOr<Value> byteIndex = emitRValue(subscript->getIdx());
  if (failed(byteIndex))
    return failure();
  return builder
      .create<emitrust::SubscriptOp>(loc, emitrust::LValueType::get(i8Type),
                                     place, *byteIndex)
      .getResult();
}

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
  // W2.28: inside an instantiated template body, a mention of a non-type
  // template parameter (`return x + N;`) is wrapped in a
  // SubstNonTypeTemplateParmExpr whose replacement is the already
  // substituted CONSTANT for this specialization. The wrapper marks the
  // substitution point for tooling; the value underneath is the whole
  // meaning, so it imports exactly like the unwrapped expression.
  if (const auto *substNttp =
          llvm::dyn_cast<clang::SubstNonTypeTemplateParmExpr>(e))
    return emitRValue(substNttp->getReplacement());
  // W2.21: a std::unique_ptr TEMPORARY (`std::make_unique<T>(a);` as a
  // statement, `*std::make_unique<T>(a)`, an argument, ...) is wrapped in a
  // CXXBindTemporaryExpr because the temporary has a destructor. There is
  // no binding for its Box to live in and therefore no drop point to place,
  // so the whole shape is out of subset; without this screen it falls to
  // the generic "unsupported expression: CXXBindTemporaryExpr" tail, which
  // names the AST node instead of the reason.
  if (const auto *bindTemp = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(e))
    if (isStdUniquePtrRecordType(bindTemp->getType()))
      return emitError(loc)
             << "unsupported: std::make_unique is only recognized as the "
                "initializer of a local std::unique_ptr variable";
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
  // anonymous enum's constant is a plain i32 value. In C++ the reference
  // HAS the enum type (both scoped and unscoped), so the enum-typed
  // constant is returned as-is (FR-113 C1): the enclosing context is
  // either enum-typed itself (assignment, return, call argument, a CK_NoOp
  // `static_cast<Color>(Blue)`) or an explicit IntegralCast node that
  // emits the enum-to-integer conversion — forcing i32 here instead made
  // `Color c = Color::Green;` reject with the misleading "assigned value
  // type does not match the place". The type test keeps the C path
  // byte-identical, because a C enumerator reference is int-typed.
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e))
    if (const auto *enumerator =
            llvm::dyn_cast<clang::EnumConstantDecl>(ref->getDecl())) {
      FailureOr<Value> constant = emitEnumConstant(enumerator, loc);
      if (failed(constant))
        return failure();
      if (llvm::isa<emitrust::EnumType>((*constant).getType()) &&
          !namedEnumDeclOf(ref->getType()))
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
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(scalarRef->getDecl())) {
      // FR-61f: a lifted range-`for` induction resolves to its `emitrust.for`
      // block-argument value directly (body-immutable per matcher clause 4),
      // with no place to load.
      if (Value induction = inductionValues.lookup(var))
        return induction;
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
  // W2.23: a call returning a copy-ctor droppy class wraps its prvalue in
  // `CXXBindTemporaryExpr` (the temporary's destructor runs at
  // full-expression end). Consumed as a value, the prvalue MOVES into its
  // consumer -- the temporary never exists apart from it, exactly C++17's
  // guaranteed elision, and a discarded one drops at the Rust statement
  // end, which IS the native full-expression end -- so the binding node
  // itself needs no code. Scoped to the admitted copy-ctor class: before
  // W2.23 no droppy value could cross a signature at all, and every other
  // dtor-carrying temporary keeps its located rejection.
  if (const auto *bindTemp = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(e))
    if (admittedCopyConstructor(bindTemp->getType()->getAsCXXRecordDecl()))
      return emitRValue(bindTemp->getSubExpr());
  if (const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(e)) {
    const clang::CXXConstructorDecl *ctor = construct->getConstructor();
    // W2.11: a std::optional VALUE construction — `return v;` /
    // `return std::nullopt;` convert through a CXXConstructExpr — routes
    // to emitStlConstruct, which yields the `Some(v)` / `None` rvalue
    // directly (no temp place needed; the opaque IS the value). This
    // interception sits BEFORE the generic trivial-copy unwrap below:
    // optional<int>'s copy ctor is trivial, so `return a;` (copy of an
    // existing optional) would otherwise slip through as a whole-value
    // load; emitStlConstruct's copy/move guard keeps it a located
    // rejection this wave instead.
    if (isStdOptionalRecordType(construct->getType())) {
      FailureOr<Type> optionalType = mapType(construct->getType(), loc);
      if (failed(optionalType))
        return failure();
      return emitStlConstruct(*optionalType, construct, loc);
    }
    // W2.14: a std::variant VALUE construction (`return 7;` in a
    // variant-returning function, a converting by-value argument) routes
    // to emitVariantConstruct, which yields the enum_variant rvalue
    // directly. Intercepted BEFORE the generic trivial-copy unwrap for
    // the same reason as optional above: the two-scalar variant's copy
    // ctor is trivial, so a copy of an existing variant would otherwise
    // slip through; emitVariantConstruct's copy/move guard keeps it a
    // located rejection this wave.
    if (isStdVariantRecordType(construct->getType())) {
      FailureOr<Type> variantType = mapType(construct->getType(), loc);
      if (failed(variantType))
        return failure();
      return emitVariantConstruct(*variantType, construct, loc);
    }
    // W2.17: a class with a user-declared destructor still has a TRIVIAL
    // implicit copy constructor at the Decl level (verified by AST dump),
    // so this unwrap fires for it and lowers a C++ whole-value COPY into a
    // Rust MOVE -- one destructor run where C++ has two. Refused here, at
    // the copy, so no value-position channel is left open.
    // W2.26: transitive -- copying a droppy-DERIVED value would likewise
    // turn C++'s two destructor runs into Rust's one.
    if (ctor && userOrInheritedDestructor(astContext(), construct->getType()))
      return emitError(loc)
             << "unsupported: value copy of a class with a destructor";
    if (ctor && ctor->isCopyOrMoveConstructor() && ctor->isTrivial() &&
        construct->getNumArgs() == 1)
      return emitRValue(construct->getArg(0));
    // W2.8: a std::pair VALUE construction (`return std::pair<int,int>(q,
    // r);`, a by-value argument, ...) materializes an anonymous temp
    // place, runs the field-wise init on it, and loads it whole —
    // mirroring the compound-literal shape above. C++17's guaranteed
    // elision means the native leg constructs the target directly; the
    // temp-and-load models the same single construction (fields assigned
    // exactly once), so the observable behavior is identical.
    if (isStdPairRecordType(construct->getType()) &&
        construct->getNumArgs() == 2) {
      FailureOr<Type> pairType = mapType(construct->getType(), loc);
      if (failed(pairType))
        return failure();
      Value place = createVariablePlace(loc, *pairType, std::string());
      if (failed(emitPairConstructInit(place, construct, loc)))
        return failure();
      return loadPlace(loc, place);
    }
    // W2.23: a value-position construction through an IMPORTED
    // user-provided constructor generalizes the W2.8 pair shape above --
    // an anonymous temp place, the ordinary constructor call on it, and a
    // whole load that MOVES the temp into its consumer. This is the
    // by-value argument (`take(a)`: 1 copy through the copy ctor) and the
    // prvalue argument / prvalue factory return (`take(T(x))`,
    // `return T(x);`: the VALUE ctor with NO copy node at all -- C++17
    // guaranteed elision as absent AST nodes, so the temp-and-move models
    // the same single construction). The W2.17 droppy refusal ABOVE stays
    // ahead of this admission: a copy+dtor class never crosses by value
    // (the measured caller-vs-callee parameter-temp drop divergence);
    // its RETURN copies are intercepted in emitReturnStmt instead. The
    // user-provided + imported gate keeps `sum(Point())` (an implicit,
    // never-imported default ctor) on the located rejection below --
    // cpp-byvalue-struct-invalid.cpp pins it.
    if (ctor && ctor->isUserProvided() &&
        functions.lookup(cxxMethodMangledName(ctor))) {
      FailureOr<Type> structType = mapType(construct->getType(), loc);
      if (failed(structType))
        return failure();
      Value place = createVariablePlace(loc, *structType, std::string());
      if (failed(emitCXXConstructInit(place, construct, loc)))
        return failure();
      return loadPlace(loc, place);
    }
    return emitError(loc) << "unsupported: constructor in value position "
                             "(only a trivial copy or move is modeled)";
  }
  // An NSDMI (`struct D { int x = 5; };`) surfaces at each use as a
  // `CXXDefaultInitExpr` standing in for the member's in-class initializer;
  // its value is exactly that initializer.
  if (const auto *defaultInit = llvm::dyn_cast<clang::CXXDefaultInitExpr>(e))
    return emitRValue(defaultInit->getExpr());
  // A defaulted call argument (`f(a)` where `f(int a, int b = 10)`) surfaces
  // at the call site as a `CXXDefaultArgExpr` standing in for the default
  // value; its value is that expression. Recursing into it also gives any
  // residual diagnostic a real source location — this node's own location is
  // invalid, which is why an unsupported default used to reject WITHOUT a
  // `file:line:col:` prefix.
  if (const auto *defaultArg = llvm::dyn_cast<clang::CXXDefaultArgExpr>(e))
    return emitRValue(defaultArg->getExpr());
  return emitError(loc) << "unsupported expression: " << e->getStmtClassName();
}

FailureOr<Value> CImporter::emitCast(const clang::CastExpr *cast) {
  Location loc = translateLoc(cast->getBeginLoc());
  const clang::Expr *sub = cast->getSubExpr();

  switch (cast->getCastKind()) {
  case clang::CK_NoOp:
    return emitRValue(sub);
  case clang::CK_ConstructorConversion:
    // W2.11: a converting-constructor conversion TO std::optional (`return
    // v;` / `return std::nullopt;` wrap their CXXConstructExpr in this
    // cast kind) is the construction itself — recurse so emitRValue's
    // optional routing sees the construct. Every other constructor
    // conversion keeps the located rejection below.
    if (isStdOptionalRecordType(cast->getType()))
      return emitRValue(sub);
    // W2.23: `return Tracer(v);` -- the prvalue factory, 00801's exact
    // shape -- wraps its CXXConstructExpr in this cast kind too; recurse
    // (mirroring the optional line above) so emitRValue's temp-place
    // branch sees the construct. Gated on an imported user-provided
    // constructor of a NON-droppy class, so the droppy prvalue factory
    // keeps this located rejection (a droppy class crosses a value
    // boundary only through the emitReturnStmt copy interception).
    if (const auto *construct =
            llvm::dyn_cast<clang::CXXConstructExpr>(sub->IgnoreParens())) {
      const clang::CXXConstructorDecl *ctor = construct->getConstructor();
      if (ctor && ctor->isUserProvided() &&
          !userOrInheritedDestructor(astContext(), construct->getType()) &&
          functions.lookup(cxxMethodMangledName(ctor)))
        return emitRValue(sub);
    }
    return emitError(loc) << "unsupported cast ("
                          << cast->getCastKindName() << ")";
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
      // FR-61f: a lifted range-`for` induction resolves to its `emitrust.for`
      // block-argument value directly (body-immutable per matcher clause 4);
      // it has no place, so the ordinary load path below would not find it.
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
        if (Value induction = inductionValues.lookup(var))
          return induction;
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
    // FR-83: an integer-scalar leaf through an opaque-union ARM reads as
    // a byte view of the blob at the leaf's clang-computed offset — the
    // same wide-byte image (`from_ne_bytes` over the window; a single
    // blob subscript for one byte). Intercepted here, before any lvalue
    // is requested: the blob has no arm-typed place.
    if (isOpaqueArmScalarLeaf(sub)) {
      FailureOr<WideByteAccess> access =
          resolveOpaqueArmByteView(sub, loc, /*writeback=*/nullptr);
      if (failed(access))
        return failure();
      return emitOpaqueArmLoad(*access, loc);
    }
    // A bit-field member read is the synthesized mask-and-shift accessor
    // over its backing field (C99-45); no lvalue of the member exists.
    if (const auto *memberExpr =
            llvm::dyn_cast<clang::MemberExpr>(stripTrivia(sub)))
      if (const auto *field =
              llvm::dyn_cast<clang::FieldDecl>(memberExpr->getMemberDecl()))
        if (field->isBitField())
          return emitBitFieldRead(memberExpr, loc);
    // W2.14: `std::get<T>(v)` returns `T&`; the value read of that
    // reference IS the match expansion (there is no place for the
    // result), so the free-function call is intercepted here before the
    // generic lvalue path below would try to form one.
    if (const clang::CallExpr *getCall = matchVariantGetCall(sub))
      return emitVariantGet(getCall);
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
      // FR-113 C2: the emitted open enum stores a fixed u32/i32, but C++'s
      // conversion to the enum truncates to the (possibly NARROWER) fixed
      // underlying type first — `static_cast<PT>(301)` with underlying
      // `unsigned char` is 45, not 301 (a measured byte-diff miscompile for
      // already-admitted unscoped `enum : unsigned char` too). An
      // intermediate cast at the underlying width reproduces the
      // truncation (and, for a signed underlying, the sign-extension back
      // into storage): rendered `Pt(v as u8 as u32)`.
      clang::QualType underlying = target->getIntegerType();
      if (unsigned width = astContext().getTypeSize(underlying); width < 32) {
        Type narrowType =
            underlying->isUnsignedIntegerType()
                ? Type(IntegerType::get(builder.getContext(), width,
                                        IntegerType::Unsigned))
                : Type(builder.getIntegerType(width));
        if ((*value).getType() != narrowType)
          value = builder.create<emitrust::CastOp>(loc, narrowType, *value)
                      .getResult();
      }
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
    // W2.21: `if (p)` / `if (!p)` / `(bool)p` over a std::unique_ptr is a
    // CK_UserDefinedConversion wrapping the `operator bool` member call, so
    // the cast rejects before the member dispatch ever names unique_ptr.
    // The reason is the wave's central boundary, not a missing cast kind.
    if (cast->getCastKind() == clang::CK_UserDefinedConversion)
      if (const auto *conversion =
              llvm::dyn_cast<clang::CXXMemberCallExpr>(sub->IgnoreImpCasts()))
        if (isStdUniquePtrRecordType(
                conversion->getImplicitObjectArgument()->getType()))
          return emitError(loc)
                 << "unsupported: a Box<T> cannot be null, so testing a "
                    "std::unique_ptr for emptiness has no image";
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
      // FR-88: a NULLABLE byte-slice parameter's null test is REAL — its
      // `None` call sites are legal — so it must never reach the
      // statically-non-null fold below. It lowers to the Option
      // discriminant method call on the parameter's lvalue place
      // (`is_none` for `==`, `is_some` for `!=`), the fn-ptr precedent's
      // spelling; the let-bound i1 keeps the emitted guard
      // clippy-clean (never inline a method call into the `if`).
      if (const clang::ParmVarDecl *param = asPointerParamRef(pointerSide);
          param && nullableByteParams.contains(param)) {
        Value place = symbols.lookup(param);
        return builder
            .create<emitrust::MethodCallOp>(
                loc, TypeRange{builder.getI1Type()}, place,
                builder.getStringAttr(isEq ? "is_none" : "is_some"),
                ValueRange{})
            .getResult(0);
      }
      // FR-99: a local bound to a NULLABLE owned-FAM allocator holds its
      // result in an Option temp until the recognized guard resolves it, so
      // its null test IS the Option discriminant (`is_none` for `==`,
      // `is_some` for `!=`) — never the statically-non-null fold below, which
      // would silently delete the guard. A test after the temp was unwrapped
      // has no discriminant left to read and rejects.
      if (const clang::VarDecl *bound = famNullableBoundLocal(pointerSide)) {
        Value temp = famOptionTemps.lookup(bound);
        if (!temp)
          return emitError(loc)
                 << "unsupported: null test of '"
                 << canonicalStreamName(bound->getName())
                 << "' outside its binding guard (a nullable "
                    "flexible-array-record allocator result is unwrapped at "
                    "the guard immediately following the binding)";
        return builder
            .create<emitrust::MethodCallOp>(
                loc, TypeRange{builder.getI1Type()}, temp,
                builder.getStringAttr(isEq ? "is_none" : "is_some"),
                ValueRange{})
            .getResult(0);
      }
      // FR-96: a LIFTED member-held FAM field's null test is REAL — `None`
      // is the unallocated/freed state — so it must never reach the
      // statically-non-null fold below. Same let-bound Option discriminant
      // shape as the FR-88 nullable parameter, on the member's place; a
      // member-read local's own null test projects the same member.
      if (const clang::MemberExpr *optMember =
              famOptionMemberOf(pointerSide)) {
        const auto *optField =
            llvm::cast<clang::FieldDecl>(optMember->getMemberDecl());
        FailureOr<Value> place = emitFamOptionMemberPlace(
            optMember, optField, loc, /*writeback=*/nullptr);
        if (failed(place))
          return failure();
        return builder
            .create<emitrust::MethodCallOp>(
                loc, TypeRange{builder.getI1Type()}, *place,
                builder.getStringAttr(isEq ? "is_none" : "is_some"),
                ValueRange{})
            .getResult(0);
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

void CImporter::scanLocaleFence(const clang::TranslationUnitDecl *unit) {
  if (!localeInstallCall.empty())
    return;
  for (const clang::Decl *decl : unit->decls()) {
    const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (!func || !func->doesThisDeclarationHaveABody())
      continue;
    SmallVector<const clang::Stmt *> worklist{func->getBody()};
    while (!worklist.empty()) {
      const clang::Stmt *current = worklist.pop_back_val();
      if (!current)
        continue;
      if (const auto *call = llvm::dyn_cast<clang::CallExpr>(current))
        if (const clang::FunctionDecl *callee = call->getDirectCallee();
            callee && callee->getDeclName().isIdentifier()) {
          llvm::StringRef name = callee->getName();
          // `setlocale` installs a locale for the whole process; `uselocale`
          // installs one for the calling thread. Either one can move the
          // `<ctype.h>` answers off the "C"-locale ASCII rule this mapping
          // was measured against, so either one fences the whole import.
          if (name == "setlocale" || name == "uselocale") {
            localeInstallCall = name.str();
            return;
          }
        }
      for (const clang::Stmt *child : current->children())
        worklist.push_back(child);
    }
  }
}

FailureOr<Value> CImporter::emitCtypeClassifier(const CtypeClassifierUse &use,
                                                Location loc) {
  FailureOr<Value> argument = emitRValue(use.arg);
  if (failed(argument))
    return failure();
  auto intType = llvm::dyn_cast<IntegerType>((*argument).getType());
  if (!intType)
    return emitError(loc) << "unsupported: <ctype.h> classifier on a "
                             "non-integer argument";
  // C requires the argument to be EOF or representable as `unsigned char`;
  // anything else is undefined. The `as u8` image is exact for the
  // `unsigned char` range and maps EOF (-1) to 255, which no classifier
  // accepts -- which is exactly what glibc answers for EOF.
  auto byteType =
      IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  Value byte = castToIntType(loc, *argument, byteType);
  neededCtypeHelpers.insert(use.helper);
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{builder.getI1Type()},
          builder.getStringAttr(use.helper), /*args=*/ArrayAttr(),
          ValueRange{byte})
      .getResult(0);
}

FailureOr<Value> CImporter::emitCondition(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParens();
  Location loc = translateLoc(e->getBeginLoc());

  // FR-129 half (b): a glibc `<ctype.h>` CLASSIFIER, admitted HERE AND
  // NOWHERE ELSE. C guarantees only "nonzero if true", but glibc's macro
  // yields the `_IS*` MASK -- a program that PRINTS `isspace(' ')` prints
  // 8192 -- so the Rust image is byte-identical only where the exact
  // nonzero value is unobservable. `emitCondition` is precisely that set:
  // `if`/`while`/`for` conditions, the `!` operand, the `&&`/`||` operands,
  // and the ternary condition. Every other use (a value, an argument, a
  // printed result) never reaches here and keeps the half-(a) located
  // ctype-table rejection.
  if (std::optional<CtypeClassifierUse> classifier =
          matchCtypeClassifier(e, astContext())) {
    if (!localeInstallCall.empty())
      return emitError(loc)
             << "unsupported: <ctype.h> classifier in a translation unit "
                "that calls '"
             << localeInstallCall
             << "' (the ASCII image is measured only in the \"C\" locale)";
    return emitCtypeClassifier(*classifier, loc);
  }

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
  // W2.26: the sole-virtual-dtor class is admitted as a value, so the fold
  // over a polymorphic type became reachable -- and it would promise the
  // vptr-carrying native layout (16) for a struct the emitter renders
  // without a vptr (4). Same screen family as the two above.
  if (typeContainsPolymorphicRecord(operand))
    return emitError(loc)
           << "unsupported: sizeof/alignof of a polymorphic class";
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

FailureOr<Value> CImporter::emitLambdaLocalCall(
    const clang::CXXOperatorCallExpr *call, const clang::VarDecl *var) {
  Location loc = translateLoc(call->getBeginLoc());
  const LambdaLocalInfo &info = lambdaLocals.find(var)->second;
  // Op handles are value-semantic; copy out of the const ref (the
  // generated accessors are non-const).
  func::FuncOp target = info.funcOp;
  FunctionType targetType = target.getFunctionType();
  unsigned frozenCount = info.frozenCaptures.size();
  // Defensive: clang already type-checked the call against operator(),
  // so the counts can only disagree if the lift built a wrong signature.
  if (call->getNumArgs() - 1 + frozenCount != targetType.getNumInputs())
    return emitError(loc) << "unsupported: call argument count mismatch";
  // The frozen capture values (loaded at the lambda's declaration point,
  // which dominates every use of the local) come first, then this call's
  // own arguments.
  SmallVector<Value> arguments(info.frozenCaptures.begin(),
                               info.frozenCaptures.end());
  for (unsigned index = 1; index < call->getNumArgs(); ++index) {
    Type input = targetType.getInput(frozenCount + index - 1);
    FailureOr<Value> value =
        emitPositionedRValue(input, call->getArg(index));
    if (failed(value))
      return failure();
    if ((*value).getType() != input)
      return emitError(loc) << "unsupported: call argument type mismatch";
    arguments.push_back(*value);
  }
  auto callOp = builder.create<func::CallOp>(loc, target, arguments);
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
}

/// W2.20: forward declaration — the `m.find(k) != m.end()` matcher is
/// defined beside the map lowering it feeds, but `emitCall` below is the
/// interception point (before any operand type is mapped).
static const clang::CXXMemberCallExpr *
matchStlFindEndIdiom(const clang::CXXOperatorCallExpr *call);

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
    // W2.22: a `std::cout`/`std::cerr` `<<` chain lowers to the Rust print
    // macros in STATEMENT position only (`emitCallStmt`). Reaching
    // `emitCall` means the chain's ostream result is being USED, and there
    // is no ostream value to hand back — the printf precedent
    // ("printf return value must be unused") applies verbatim.
    {
      llvm::StringRef stream;
      llvm::SmallVector<const clang::CXXOperatorCallExpr *> links;
      if (matchOstreamChain(opCall, stream, links))
        return emitError(loc) << "unsupported: the result of a std::ostream "
                                 "<< chain must be unused";
    }
    // W2.20: `m.find(k) != m.end()` is the ONE std::map/std::set iterator
    // shape the wave admits, and it lowers to `contains_key(&k)` without
    // materializing an iterator. It must be intercepted HERE, before any
    // operand type is mapped: `operator!=` over map iterators is a FREE
    // function template (so the std-member dispatch below never sees it),
    // and mapping either operand's type would reject on the iterator
    // first.
    if (const clang::CXXMemberCallExpr *findCall =
            matchStlFindEndIdiom(opCall))
      return emitStlFindEndTest(opCall, findCall);
    const auto *opMethod = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(
        opCall->getDirectCallee());
    if (opMethod && opMethod->getParent()->isInStdNamespace())
      return emitStlOperatorCall(opCall);
    // W2.21: `p == nullptr` / `nullptr != p` over a std::unique_ptr
    // resolve to a FREE ADL `operator==`/`operator!=` in namespace std, so
    // neither the member dispatch above nor the identifier-named
    // free-function dispatch below ever sees them (an operator has no
    // identifier name, so the generic path rejects it as "unsupported
    // callee" and hides the real reason). A Rust `Box<T>` cannot be null
    // and the wave admits only the always-initialized subset, so the null
    // test names exactly that.
    if (const clang::FunctionDecl *opFn = opCall->getDirectCallee();
        opFn && opFn->isInStdNamespace())
      for (const clang::Expr *arg : opCall->arguments())
        if (isStdUniquePtrRecordType(arg->getType()))
          return emitError(loc)
                 << "unsupported: a Box<T> cannot be null, so comparing a "
                    "std::unique_ptr against nullptr has no image";
    // W2.13: a direct `f(args)` operator() call on a RECOGNIZED lifted
    // lambda local rewrites to `lifted(frozen..., args...)`. Any other
    // lambda call shape (an unregistered local means its declaration
    // already rejected; an immediately-invoked lambda expression was
    // never a local at all) falls through to the located "unsupported
    // callee" rejection below.
    if (opMethod && opMethod->getParent()->isLambda() &&
        opCall->getOperator() == clang::OO_Call && opCall->getNumArgs() >= 1)
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
              opCall->getArg(0)->IgnoreParenImpCasts()))
        if (const auto *lambdaVar =
                llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
          if (lambdaLocals.contains(lambdaVar))
            return emitLambdaLocalCall(opCall, lambdaVar);
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
  if (!callee->getDeclName().isIdentifier()) {
    // FR-112: a MEMBER overloaded operator is OMITTED from its class's
    // import (the class itself stays importable), so its use sites are the
    // only place the construct can be reported -- and they must be rankable.
    // The wording names the omitted member and its class, and it is the
    // needle of the `cxx-omitted-member` ledger tag (ordered ABOVE the
    // "overloaded operator" needle in lib/ImportC/RejectionLedger.cpp and
    // test/RealWorld/run_realworld.py, since this message contains both
    // substrings). Every spelled use (`a + b`, `b = a`) and the implicit
    // ENCLOSING-`operator=` channel (a member with a user `operator=` makes
    // the enclosing class's implicit copy-assignment non-trivial, so
    // `outer1 = outer2` arrives here as a CXXOperatorCallExpr naming it)
    // lands on this guard -- though since W2.23 the statement-position
    // implicit copy-assignment is intercepted in emitCallStmt first
    // (memberwise for scalar fields, an honest located rejection
    // otherwise), so only value uses of it still reach here, and they
    // take the implicit-specific wording below. Lambdas keep the generic wording (their
    // `operator()` is the closure-call frontier, not an omitted member),
    // and so do std-namespace records (their methods are intercepted, never
    // imported, so "omitted" would misreport the boundary).
    if (const auto *methodCallee =
            llvm::dyn_cast<clang::CXXMethodDecl>(callee);
        methodCallee && methodCallee->isOverloadedOperator() &&
        !methodCallee->getParent()->isLambda() &&
        !methodCallee->getParent()->isInStdNamespace()) {
      // W2.23: the IMPLICIT copy-assignment operator was never "omitted"
      // -- statement position now lowers it memberwise
      // (emitImplicitCopyAssign) -- so the residual VALUE use (`(a = b)`
      // consumed for its T& result) says what it is instead of borrowing
      // the omitted-member wording below.
      if (methodCallee->isImplicit() &&
          methodCallee->isCopyAssignmentOperator())
        return emitError(loc)
               << "unsupported: implicit copy assignment in value position";
      // W2.25: a member operator of an ADMITTED kind whose method actually
      // IMPORTED (its `<Struct>_op_*` func exists — the class walk may
      // still have omitted it for a shape failure, e.g. a reference
      // return) lowers through the method-call machinery with argument 0
      // as the receiver. The lookup gate keeps every OMITTED operator on
      // the honest FR-112 wording below, so still-fenced shapes never
      // silently change their diagnostic.
      if (const auto *memberOpCall =
              llvm::dyn_cast<clang::CXXOperatorCallExpr>(call);
          memberOpCall && !methodCallee->isImplicit() &&
          memberOpCall->getNumArgs() >= 1 &&
          !emitrust::operatorSymbolBaseName(methodCallee).empty())
        if (func::FuncOp target =
                functions.lookup(cxxMethodMangledName(methodCallee)))
          return emitCXXOperatorMemberCall(memberOpCall, methodCallee,
                                           target);
      const clang::RecordDecl *ownerDefinition =
          methodCallee->getParent()->getDefinition();
      std::string ownerName =
          ownerDefinition ? recordRustName(ownerDefinition) : std::string();
      return emitError(loc)
             << "unsupported: call to overloaded operator '"
             << callee->getDeclName().getAsString() << "' omitted from class '"
             << (ownerName.empty() ? llvm::StringRef("<anonymous>")
                                   : llvm::StringRef(ownerName))
             << "'";
    }
    // W2.25: an admitted FREE operator resolves to its synthesized
    // FR-114-suffixed symbol and lowers through the ordinary free-function
    // dispatch below — the same borrow-argument machinery, the same
    // reconciliation, byte-identical to the renamed-twin differential the
    // wave's spike measured. std-namespace and system-header operators
    // stay out (their types are STL territory and their defs never
    // import), as do lambdas' `operator()` and every member operator
    // (handled or rejected above), so nothing changes its wording here
    // but the admitted free shapes.
    if (!llvm::isa<clang::CXXMethodDecl>(callee) &&
        !callee->isInStdNamespace() && !isSystemHeaderDecl(callee) &&
        !emitrust::operatorSymbolBaseName(callee).empty()) {
      // Falls through to the ordinary dispatch below.
    } else {
      return emitError(loc) << "unsupported callee";
    }
  }
  // W2.14: the std::variant FREE-function vocabulary — the first
  // std-namespace free-function interception in this dispatch (members
  // and operators divert above; a free `std::get` would otherwise fall
  // to the unspecific system-header/unimported-function rejections).
  // `std::get<T>` over a recognized variant expands to its RESULT-mode
  // match; holds_alternative and visit stay located rejections this wave
  // (no alternative-state tracker exists, and visit's callable dispatch
  // has no image). Gated on the argument's record shape so the
  // same-named functions over pairs/tuples/arrays keep their historical
  // diagnostics.
  if (callee->isInStdNamespace()) {
    llvm::StringRef stdName = callee->getName();
    if (call->getNumArgs() >= 1 &&
        isStdVariantRecordType(call->getArg(0)->getType())) {
      if (stdName == "get")
        return emitVariantGet(call);
      if (stdName == "holds_alternative")
        return emitError(loc) << "unsupported: std::holds_alternative is "
                                 "not a recognized STL function";
    }
    if (stdName == "visit")
      return emitError(loc)
             << "unsupported: std::visit is not a recognized STL function";
    // W2.21: the std::unique_ptr FREE-function vocabulary.
    //
    // `std::make_unique` is admitted ONLY as the initializer of a local
    // std::unique_ptr variable (`emitStlBoxLocalInit`); every other
    // position — an argument, a return, a bare statement — would need an
    // owned Box temporary with no binding to drop it at the right point.
    //
    // `std::move` is THE unique_ptr idiom and Rust's move semantics match
    // it exactly, but only in one direction: C++ leaves the moved-from
    // unique_ptr NULL and testable (`q = std::move(p); if (!p)` is
    // well-defined and prints something), and Rust cannot read a moved-from
    // binding at all. Since the wave's Box image has no null state, the
    // moved-from OBSERVATION has no representation, so the transfer
    // rejects rather than silently dropping the observable.
    if (stdName == "make_unique" || stdName == "make_unique_for_overwrite")
      return emitError(loc)
             << "unsupported: std::make_unique is only recognized as the "
                "initializer of a local std::unique_ptr variable";
    if (stdName == "move" || stdName == "forward")
      for (const clang::Expr *arg : call->arguments())
        if (isStdUniquePtrRecordType(arg->getType()))
          return emitError(loc)
                 << "unsupported: a moved-from std::unique_ptr is null and "
                    "testable, but Rust cannot read a moved-from binding";
  }
  // W2.25: an admitted free operator reaches this dispatch with a
  // non-identifier DeclarationName, where `getName()` would assert; every
  // by-name interception below reads this guarded spelling instead (empty
  // for an operator, so none of them can fire on one).
  llvm::StringRef calleeName = callee->getDeclName().isIdentifier()
                                   ? callee->getName()
                                   : llvm::StringRef();
  // The hosted (definition-less) printf lowering is statement-position
  // only; a project-supplied printf definition is an ordinary imported
  // function whose result is an ordinary value in every position.
  if (calleeName == "printf" && !callee->getDefinition())
    return emitError(loc) << "unsupported: printf return value must be unused";
  // Statement-position puts/putchar are lowered by name (emitCallStmt);
  // their int result has no representation there, so a value use of a
  // definition-less puts/putchar is rejected.
  if ((calleeName == "puts" || calleeName == "putchar") &&
      !callee->getDefinition())
    return emitError(loc) << "unsupported: " << calleeName
                          << " return value must be unused";
  // A definition-less strlen is lowered by name like printf/puts: its
  // supported argument shape is a pointer into a string-literal region,
  // rendered through the `__emitrust_strlen` helper. A user-defined strlen
  // is an ordinary call.
  if (calleeName == "strlen" && !callee->getDefinition())
    return emitStrlenCall(call);
  // Hosted <string.h> comparisons (design.md C99-48, CTS-L1) are lowered
  // by name, like strlen; their int result is an ordinary value. The
  // copy/fill functions of the same surface are statement-position only
  // (their char* result has no decomposed representation), and a
  // strchr/strrchr result is consumed by the printf %s and null-comparison
  // interceptions before reaching this point.
  if (!callee->getDefinition()) {
    llvm::StringRef name = calleeName;
    // A definition-less sprintf is lowered by name (design.md CTS-P9,
    // 00186): the literal format translates through the shared printf
    // grammar into a `format!` String and the `__emitrust_sprintf`
    // helper writes it into the destination char region, returning the
    // length. Statement-position calls arrive here through emitCallStmt's
    // emitCall fallthrough and simply discard the value.
    if (name == "sprintf")
      return emitSprintf(call);
    // snprintf(dest, size, fmt, ...) shares the sprintf lowering but honors
    // the size bound via the truncating `__emitrust_snprintf` helper.
    if (name == "snprintf")
      return emitSprintf(call, /*isSnprintf=*/true);
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
          canonicalStreamName(streamDecl->getName()) != "stdout")
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
    // FR-112: a STATIC METHOD with no imported func can only be a member
    // the class's import OMITTED (the FR-47 prepass registers every
    // importable method before any body runs), so it takes the method
    // wording -- which the `cxx-omitted-member` ledger needle classifies --
    // rather than the C "unimported function" one. This import-level
    // rejection is ALL there is for a static: a resolved static call
    // renders as an opaque callee string the verifier never checks
    // (design.md FR-112, constraint C8), so a miss here would surface only
    // as rustc E0599 at cargo time.
    if (staticMethod)
      return emitError(loc) << "unsupported: call to unimported method '"
                            << name << "'";
    return emitError(loc) << "unsupported: call to unimported function '"
                          << name << "'";
  }

  // A method-planned callee (Phase 4) takes the owner receiver plus i64
  // element cursors in place of its pointer arguments.
  if (const clang::VarDecl *ownerBase =
          methodPlans.lookup(callee->getCanonicalDecl()))
    return emitMethodCallSite(call, target, ownerBase, loc);

  // A callee with planned cursor parameters (CTS 00204 / C99-43
  // slice 1 / C1) expands each `&p` Shape-S argument into (shared
  // region slice, in-out cursor), each `&e` Shape-P argument into a
  // staged out-cursor temp, and each `&p` Shape-G argument into a
  // staged `Option<i64>` cell, storing the advanced/written state back
  // after the call.
  if (const clang::FunctionDecl *definition = callee->getDefinition();
      definition && llvm::any_of(definition->parameters(),
                                 [&](const clang::ParmVarDecl *param) {
                                   return cursorParams.contains(param) ||
                                          pairedCursorParams.contains(param) ||
                                          globalCursorParams.contains(param);
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
    // FR-88: a NULLABLE byte-slice parameter (`Option<&[u8]>`). C's null
    // pointer constant is the `None` value — an inline constant, exactly
    // like a fn-ptr `None` — and every other argument resolves as a
    // shared byte-region borrow in the borrow phase below, wrapped in
    // `Some(...)` there; a region-less argument keeps its located
    // rejection inside `emitBorrowArgument`, never a silent one-element
    // borrow.
    if (isNullableByteSliceType(input)) {
      if (isNullPointerConstantExpr(argument)) {
        arguments[index] = createFnPtrNone(loc, input);
        continue;
      }
      borrows.push_back({index, argument});
      continue;
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
    // FR-94: an OWNED-RECORD parameter (the free-only wrapper's by-value
    // FAM record) takes its pointer argument as a MOVE of the caller's
    // owned FAM local — `dec_free(d)` loads (moves) `d`, so a later use is
    // rustc E0382, the loud direction. Any other pointer argument to an
    // owned struct slot has no owner to move from and rejects located.
    if (llvm::isa<emitrust::StructType>(input) &&
        isDataPointer(argument->getType())) {
      const clang::Expr *peeled = stripTrivia(argument);
      while (const clang::Expr *sub = peelPointerCast(astContext(), peeled))
        peeled = stripTrivia(sub);
      const clang::VarDecl *owned = asLoadedLocalVarRef(peeled);
      Value ownedPlace = owned ? symbols.lookup(owned) : Value();
      if (owned && famAllocLocals.contains(owned) && ownedPlace &&
          ownedPlace.getType() == Type(emitrust::LValueType::get(input))) {
        arguments[index] =
            builder.create<emitrust::LoadOp>(loc, input, ownedPlace)
                .getResult();
        continue;
      }
      return emitError(loc)
             << "unsupported: pointer argument to an owned "
                "flexible-array-record parameter is not an owned local";
    }
    // Positioned against the callee's input: a refined
    // (callsite-inferred, FR-29 / CTS 00209) fn-ptr parameter binds a
    // directly-referenced function at the refined signature.
    FailureOr<Value> value = emitPositionedRValue(input, argument);
    if (failed(value))
      return failure();
    arguments[index] = *value;
  }

  // FR-93: EXACTLY ONE multi-base pointer borrow argument (the aes cbc
  // `XorWithIv(buf, Iv)` shape) dispatches the WHOLE call on the
  // argument's enum-of-bases discriminant — no single region base
  // exists, so each arm materializes its base's slice view and repeats
  // the call. Scope is deliberately narrow: a direct void callee, no
  // cell-slice/nullable inputs, one multi-base argument — everything
  // else (two multi-base arguments, a result-carrying callee) falls
  // through to `emitBorrowArgument`'s located multi-base rejection.
  if (!astContext().getLangOpts().CPlusPlus && !staticMethod && !vaClone &&
      cellGlobals.empty() && targetType.getNumResults() == 0 &&
      !borrows.empty()) {
    unsigned multiCount = 0;
    unsigned multiIndex = 0;
    const clang::VarDecl *multiVar = nullptr;
    bool nullableSlotSeen = false;
    for (const PendingBorrow &borrow : borrows) {
      if (isNullableByteSliceType(targetType.getInput(borrow.index)))
        nullableSlotSeen = true;
      if (const clang::VarDecl *var = asMultiBasePointerRead(borrow.expr)) {
        multiCount++;
        multiIndex = borrow.index;
        multiVar = var;
      }
    }
    if (multiCount == 1 && !nullableSlotSeen) {
      SmallVector<std::pair<unsigned, const clang::Expr *>, 4> borrowList;
      for (const PendingBorrow &borrow : borrows)
        borrowList.push_back({borrow.index, borrow.expr});
      if (failed(emitMultiBaseCallDispatch(call, target, loc, arguments,
                                           borrowList, multiIndex,
                                           multiVar)))
        return failure();
      return Value();
    }
  }

  // Borrow-producing arguments: each resolves to a fresh borrow of its
  // region base. Two borrows of the same base would alias mutably in Rust;
  // they are rejected rather than emitted. FR-74: a member-array argument
  // borrows only its FIELD, so the collision key is (base, member path)
  // with PREFIX-overlap semantics — the same field twice collides, a
  // whole-object borrow (empty path) collides with any member of the same
  // base, and DISJOINT sibling fields of one struct are admitted: their
  // storage is non-overlapping in C, and the emitted two-simultaneous-&mut
  // shape over sibling fields is legal Rust (rustc-verified in the FR-74
  // spike). Non-member borrows keep an empty path, preserving the
  // historical whole-object collision behavior byte-for-byte.
  SmallVector<std::pair<const clang::VarDecl *,
                        SmallVector<const clang::FieldDecl *, 2>>,
              4>
      borrowRoots;
  for (const PendingBorrow &borrow : borrows) {
    const clang::VarDecl *root = nullptr;
    SmallVector<const clang::FieldDecl *, 2> rootPath;
    // FR-88: a nullable parameter's argument borrows the SHARED byte
    // slice its Option wraps — the ordinary `&[u8]` slice-argument
    // machinery, region views, aliasing keys and rejections included —
    // and the reference is then wrapped in `Some(...)` below.
    Type input = targetType.getInput(borrow.index);
    bool nullableSlot = isNullableByteSliceType(input);
    Type borrowType =
        nullableSlot
            ? Type(emitrust::RefType::get(
                  emitrust::SliceType::get(IntegerType::get(
                      builder.getContext(), 8, IntegerType::Unsigned))))
            : input;
    FailureOr<Value> reference =
        emitBorrowArgument(loc, borrow.expr, borrowType, root, &rootPath);
    if (failed(reference))
      return failure();
    // FR-104: an armed parameter-cursor-return capture records the rooted
    // argument's region base and reslice cursor for `emitPointerRValue`'s
    // CallExpr arm — but only for a whole-object caller-LOCAL region (an
    // empty member path) whose borrow is the ordinary `slice_of`: a
    // string-literal backing, a staged global copy, or a member window
    // has no whole-region place the returned cursor could re-index, so
    // the capture stays unfilled and the arm rejects located.
    if (paramCursorCallCapture.call == call &&
        paramCursorCallCapture.argIndex == borrow.index && root &&
        rootPath.empty() && root->hasLocalStorage() && !nullableSlot) {
      if (auto sliceOf =
              (*reference).getDefiningOp<emitrust::SliceOfOp>()) {
        paramCursorCallCapture.base = root;
        paramCursorCallCapture.cursor = sliceOf.getIndex();
      }
    }
    if (root) {
      for (const auto &held : borrowRoots) {
        if (held.first != root)
          continue;
        size_t common = std::min(held.second.size(), rootPath.size());
        if (llvm::ArrayRef(held.second).take_front(common) ==
            llvm::ArrayRef(rootPath).take_front(common))
          return emitError(loc)
                 << "unsupported: aliasing mutable pointer arguments (two "
                    "arguments borrow object '"
                 << root->getName() << "')";
      }
      borrowRoots.push_back({root, rootPath});
    }
    if (nullableSlot)
      reference = builder
                      .create<emitrust::CallOpaqueOp>(
                          loc, TypeRange{input},
                          builder.getStringAttr("Some"),
                          /*args=*/ArrayAttr(), ValueRange{*reference})
                      .getResult(0);
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
    // By-value lookup: a StringRef binding would dangle (see
    // cxxMethodMangledName).
    std::string structName =
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
  // W2.24: a call to a can-throw-closure member returns the synthesized
  // carrier; unwrap it here (two RESULT-mode matches + the early-return
  // cf pattern — see unwrapThrowsResult). The planner's closure gates
  // (arithmetic parameters only, payload-typed return) guarantee such a
  // callee never took the cursor/owner/cell-slice call paths above, so
  // this generic call op is the only one that can carry the carrier.
  if (throwsPlanActive &&
      throwsClosure.contains(callee->getCanonicalDecl()))
    return unwrapThrowsResult(loc, callOp->getResult(0));
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

// W2.19b: whether `candidate` is `target` or overrides it, transitively —
// the C++ final-overrider relation restricted to the single-inheritance
// chains the import subset admits.
static bool overridesMethodTransitively(const clang::CXXMethodDecl *candidate,
                                        const clang::CXXMethodDecl *target) {
  if (candidate->getCanonicalDecl() == target->getCanonicalDecl())
    return true;
  for (const clang::CXXMethodDecl *overridden :
       candidate->overridden_methods())
    if (overridesMethodTransitively(overridden, target))
      return true;
  return false;
}

// W2.19b: resolves the FINAL OVERRIDER of virtual `method` for an object
// whose most-derived class is `mostDerived` — the devirtualization target
// (the spike's resolved-override approach). Walks the single-base chain
// DOWNWARD from the most-derived class and returns the first method that
// is (or transitively overrides) `method`; reaching `method`'s own class
// with no override in between yields `method` itself. Returns null on any
// shape outside the walk (a multi-base class before the declaring class
// is found), which callers must keep fenced — never guess a binding.
static const clang::CXXMethodDecl *
resolveDevirtualizedTarget(const clang::CXXMethodDecl *method,
                           const clang::CXXRecordDecl *mostDerived) {
  const clang::CXXRecordDecl *methodClass =
      method->getParent()->getCanonicalDecl();
  const clang::CXXRecordDecl *record = mostDerived->getDefinition();
  int depth = 0;
  while (record) {
    for (const clang::CXXMethodDecl *candidate : record->methods())
      if (candidate->isVirtual() &&
          overridesMethodTransitively(candidate, method))
        return candidate;
    if (record->getCanonicalDecl() == methodClass)
      return method;
    if (record->getNumBases() != 1 || ++depth > 64)
      return nullptr;
    record = record->bases_begin()->getType()->getAsCXXRecordDecl();
    record = record ? record->getDefinition() : nullptr;
  }
  return nullptr;
}

FailureOr<Value>
CImporter::emitCXXMemberCall(const clang::CXXMemberCallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  const clang::CXXMethodDecl *method = call->getMethodDecl();
  // W2.19a split this gate: `method->isVirtual()` no longer rejects here.
  // On a VALUE receiver the static bind below is exact C++ semantics
  // (static type == dynamic type), and `getMethodDecl()` already names
  // the receiver's own type's override; the pointer-shaped receivers,
  // where the bind would be an assumption, are fenced in the receiver
  // lambda below. The wording is retained for the residual !method arm
  // (a callee clang could not resolve to a concrete method).
  if (!method)
    return emitError(loc) << "unsupported: virtual or unresolved member call";
  // W2.3: a method declared in namespace `std` (`std::vector<T>`'s /
  // `std::string`'s own inherent methods) never has an imported func — no
  // libstdc++ method is ever imported — so it is intercepted here, before
  // the generic imported-method lookup below would reject it as "call to
  // unimported method".
  if (method->getParent()->isInStdNamespace())
    return emitStlMemberCall(call);
  // W2.21: `p->m()` / `(*p).m()` over a recognized std::unique_ptr. The
  // method belongs to the PAYLOAD class, so the implicit object argument
  // is the `operator->`/`operator*` call and the generic path below would
  // hand `emitLValue` an expression with no struct place.
  //
  // It lowers to an `emitrust.method_call` DIRECTLY on the Box place
  // (Rust's auto-deref resolves `p.node_bump(2)` for a `Box<Node>`), NOT
  // through the payload borrow the field places take. The split is
  // load-bearing, not cosmetic: the generic path builds the receiver's
  // `&mut` BEFORE the argument values, so `p->bump(p->get())` through a
  // borrowed receiver is rustc E0502 (measured in the spike). Arguments
  // emitted first and bound as ordinary values cannot collide.
  if (const clang::Expr *boxBase =
          matchStlBoxDerefBase(call->getImplicitObjectArgument())) {
    // W2.19a: the Box interception sits ABOVE the receiver lambda, so it
    // would bypass the pointer fence below -- and a unique_ptr IS a
    // pointer-shaped receiver. Today the payload type is exact (only the
    // same-T `std::make_unique<T>` initializer is recognized, so no
    // derived object can hide behind a `unique_ptr<Base>`), which would
    // make the static bind accidentally correct. W2.19b DELIBERATELY did
    // not extend devirtualization here: its gate is the single-object
    // REGION fact, and a Box has no region -- a Box bind's soundness
    // would rest solely on the W2.21 same-T recognition invariant, which
    // no byte-diff oracle guards. If this fence is ever lifted, that
    // coupling must be pinned first (an admitted `unique_ptr<Base>` from
    // `make_unique<Derived>` would turn the lift into a silent
    // miscompile). Reject rather than lean on an unguarded invariant.
    if (method->isVirtual())
      return emitError(loc)
             << "unsupported: virtual method call through a pointer";
    return emitStlBoxMethodCall(call, boxBase, loc);
  }
  // W2.18: the implicit object argument of an INHERITED call is wrapped in
  // an implicit derived-to-base conversion, which is NOT a
  // qualification-only adjustment -- it names a different object (the base
  // subobject). The hops are peeled here, ahead of the imported-method
  // lookup, so that an access through an EMPTY base (which carries no
  // `base` field at all, see `collectRecordFields`) reports the real
  // blocker instead of the lookup's consequential "call to unimported
  // method" wording.
  llvm::SmallVector<clang::QualType, 2> baseHops;
  const clang::Expr *receiverExprPeeled =
      peelDerivedToBaseCasts(call->getImplicitObjectArgument(), baseHops);
  for (clang::QualType hop : baseHops) {
    const clang::CXXRecordDecl *hopRecord = hop->getAsCXXRecordDecl();
    if (hopRecord && hopRecord->hasDefinition() && hopRecord->isEmpty())
      return emitError(loc)
             << "unsupported: inherited member of an empty base class";
  }
  // FR-117: a member whose `DeclarationName` is not an ordinary identifier
  // is OMITTED from the class's import, so it has no symbol. Reject at the
  // call rather than falling through to the lookup below: the mangled
  // spelling of an omitted member is `<Struct>_` with an EMPTY base name, so
  // the generic "call to unimported method" wording would leak `'C_'` and
  // name nothing. The explicit `c.operator int()` spelling is the one
  // conversion-function channel that USED to work (it lowered to a correct
  // call to the empty-named method); withdrawing it is what buys the class
  // importing at all, and it is pinned as a deliberate frontier move in
  // test/Import/Cpp/cpp-conversion-function-invalid.cpp.
  // W2.25: the explicit member spelling of an ADMITTED, actually-imported
  // operator (`s.operator==(o)`) resolves through the same synthesized
  // `<Struct>_op_*` symbol the operator syntax uses, so it falls through
  // to the ordinary member-call lowering below. The imported-func gate
  // keeps every OMITTED operator (non-admitted kind, or an admitted kind
  // whose shape failed, e.g. a reference return) on FR-117's wording.
  if (!method->getDeclName().isIdentifier() &&
      !llvm::isa<clang::CXXConstructorDecl>(method) &&
      !llvm::isa<clang::CXXDestructorDecl>(method) &&
      (emitrust::operatorSymbolBaseName(method).empty() ||
       !functions.lookup(cxxMethodMangledName(method))))
    return emitError(loc) << (llvm::isa<clang::CXXConversionDecl>(method)
                                  ? "unsupported: conversion function"
                                  : "unsupported: overloaded operator");
  // W2.19b: DEVIRTUALIZATION, ahead of the mangled-name lookup because a
  // successful resolution REPLACES the callee. The pointer-shaped
  // receiver classification is hoisted out of the receiver lambda below
  // so the gate and the fence see the same expression. A VIRTUAL call
  // through a pointer whose region binds EXACTLY ONE local object (the
  // FR-120 single-object fact, `singleObjectLocalPointerBase`) resolves
  // at compile time against that object's most-derived type: the object
  // IS the pointee, so its dynamic type is statically known and the
  // final overrider is exact C++ semantics — no dispatch machinery, no
  // new ops (byte-diffed in the W2.19 spike; the oracle is
  // test/EndToEnd/cpp-devirt-base-pointer.cpp). The receiver then
  // reconciles toward the devirt TARGET's class, not the pointer's
  // static pointee (FR-120's carry-forward): an override binds the
  // derived place with NO hop, an inherited virtual binds the declaring
  // class's place through the recomputed hop chain. A QUALIFIED callee
  // (`p->B::f()`) never devirtualizes — C++ binds it statically, so the
  // override would be the WRONG body; it keeps the fence below. The
  // NON-virtual half of the legD4 rule needs no code here: it already
  // binds the pointer's STATIC type through the ordinary reconcile.
  const clang::Expr *receiverStripped =
      receiverExprPeeled->IgnoreParenImpCasts();
  // `p->m()`: the implicit object argument IS the pointer (keep the
  // implicit casts on — `isDecomposedPointerExpr`'s parameter check
  // needs the LValueToRValue wrapper).
  const clang::Expr *pointerExpr = nullptr;
  if (isDataPointer(receiverStripped->getType()))
    pointerExpr = receiverExprPeeled;
  // `(*p).m()`: peel the explicit dereference to the pointer.
  else if (const auto *unary =
               llvm::dyn_cast<clang::UnaryOperator>(receiverStripped);
           unary && unary->getOpcode() == clang::UO_Deref &&
           isDataPointer(unary->getSubExpr()->getType()))
    pointerExpr = unary->getSubExpr();
  bool devirtualized = false;
  clang::QualType devirtReceiverClass;
  if (pointerExpr && method->isVirtual()) {
    const auto *callee =
        llvm::dyn_cast<clang::MemberExpr>(call->getCallee()->IgnoreParens());
    if (!(callee && callee->hasQualifier()))
      if (const clang::VarDecl *object =
              singleObjectLocalPointerBase(pointerExpr))
        if (const clang::CXXRecordDecl *mostDerived =
                object->getType().getCanonicalType()->getAsCXXRecordDecl())
          if (const clang::CXXMethodDecl *resolved =
                  resolveDevirtualizedTarget(method, mostDerived)) {
            clang::QualType targetClass =
                astContext().getRecordType(resolved->getParent());
            // The receiver hop chain (most-derived -> declaring class)
            // must exist for the inherited-virtual case; refusing here
            // keeps the fence, never a wrong bind.
            if (resolved->getParent()->getCanonicalDecl() ==
                    mostDerived->getCanonicalDecl() ||
                uniquePublicSingleBaseChain(object->getType(), targetClass,
                                            /*allowPolymorphic=*/true)) {
              method = resolved;
              devirtReceiverClass = targetClass;
              devirtualized = true;
              // Devirt owns the WHOLE receiver projection: the reconcile
              // walks from the bound object's most-derived class to the
              // target's class. The call-site hops peeled above describe
              // the POINTER's static view (`pd->geta()` for a `D2 *`
              // wraps a pointer-form DerivedToBase cast) and replaying
              // them on the reconciled place would double-project
              // (measured: `member ["base"]` on the base's own place --
              // no field-existence check catches it before rustc).
              baseHops.clear();
            }
          }
  }
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
  //
  // The derived-to-base hops peeled above are replayed as explicit
  // `member ["base"]` projections below: `IgnoreParenImpCasts` strips that
  // conversion SILENTLY, so before W2.18 an inherited call would have
  // borrowed the DERIVED place and handed it to the base method's `&Base`
  // parameter.
  // FR-120: a receiver reached through a struct POINTER (`p->m()`,
  // `(*p).m()`) resolves through the same pointer-place machinery
  // `emitMemberBasePlace`'s `->` branch uses for `p->x`, instead of
  // landing on the pointer decl inside `emitLValue` (whose blanket
  // "unsupported use of pointer variable" rejection used to fire here).
  // E0502 cannot recur structurally on this path (unlike W2.21's Box
  // receiver, intercepted above): each argument renders as its own `let`
  // and the receiver borrow is an inline autoref under two-phase
  // borrows, so `p->bump(p->get())` is measured clean.
  FailureOr<Value> receiver = [&]() -> FailureOr<Value> {
    // W2.19a's soundness fence, NOT a defensive check: a VIRTUAL call is
    // admitted on a value receiver, where the static bind is exact, and
    // (since W2.19b) on a pointer the devirtualization gate above
    // resolved -- where the single-object region fact makes the dynamic
    // type static. Through every OTHER pointer-shaped receiver the
    // pointee's dynamic type is an assumption, and the fence sits HERE
    // -- after `pointerExpr` is computed -- so every pointer spelling
    // funnels through one check: `p->f()`, `(*p).f()`, a pointer
    // parameter, and explicit or IMPLICIT `this` (a `CXXThisExpr` is
    // pointer-typed). The `this` case is the measured miscompile the
    // W2.19a spike pinned: without it, `d.callf()` -- where `B::callf`
    // returns `this->f()` and `D` overrides `f` -- compiles clean and
    // prints the BASE's answer, because the call-site hop projection
    // upcasts the receiver in a way `uniquePublicSingleBaseHops` never
    // sees. Shapes the fence still deliberately over-rejects: a
    // same-type pointer parameter (unsound across callers in principle),
    // a ctor-body virtual call (C++ ctor semantics equal the static bind
    // -- a future carve-out), a qualified `p->B::f()` (the static bind
    // is a future carve-out; devirt would be WRONG), and nullable /
    // multi-object / global-object regions (no single-object fact).
    // Inside an importable method body this rejection rides the FR-112
    // omission channel like any other body failure; in a constructor it
    // is fatal at the call, the explicit ctor rejection W2.19's spike
    // record asked for.
    if (pointerExpr && method->isVirtual() && !devirtualized)
      return emitError(loc)
             << "unsupported: virtual method call through a pointer";
    if (!pointerExpr)
      return emitLValue(receiverStripped);
    const clang::VarDecl *regionBase = nullptr;
    FailureOr<Value> place =
        emitStructPointerPlace(pointerExpr, loc, /*writeback=*/nullptr,
                               &regionBase, devirtReceiverClass);
    if (failed(place))
      return failure();
    // A GLOBAL region base resolves through a staged copy, and the call
    // path has no writeback machinery: a mutating method's effect on the
    // copy would be silently dropped. Reject rather than miscompile.
    if (regionBase && !regionBase->hasLocalStorage() && !method->isConst())
      return emitError(loc) << "unsupported: mutating method call through a "
                               "pointer to a global object";
    return place;
  }();
  if (failed(receiver))
    return failure();
  auto receiverLValueType =
      llvm::dyn_cast<emitrust::LValueType>((*receiver).getType());
  if (!receiverLValueType ||
      !llvm::isa<emitrust::StructType>(receiverLValueType.getValueType()))
    return emitError(loc)
           << "unsupported: member call receiver is not a struct place";
  // W2.19b: a devirtualized receiver was already reconciled to the devirt
  // TARGET's class inside `emitStructPointerPlace` (the recomputed
  // most-derived -> declaring-class chain); the hops peeled from the AST
  // describe that SAME conversion, so replaying them here would project a
  // second `member ["base"]` off a place that has no such field.
  if (!baseHops.empty() && !devirtualized) {
    FailureOr<Value> baseReceiver = projectBaseHops(*receiver, baseHops, loc);
    if (failed(baseReceiver))
      return failure();
    receiver = baseReceiver;
  }

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

FailureOr<Value> CImporter::emitCXXOperatorMemberCall(
    const clang::CXXOperatorCallExpr *call, const clang::CXXMethodDecl *method,
    func::FuncOp target) {
  Location loc = translateLoc(call->getBeginLoc());
  FunctionType targetType = target.getFunctionType();
  // A member operator's `CXXOperatorCallExpr` carries the receiver as
  // argument 0 and the operator's parameters after it, which is exactly
  // the imported signature's shape (receiver reference first).
  if (call->getNumArgs() != targetType.getNumInputs())
    return emitError(loc) << "unsupported: call argument count mismatch";
  // W2.18 mirror: the receiver argument of an INHERITED operator wears an
  // implicit derived-to-base conversion naming the base subobject; peel
  // the hops and replay them as explicit `member ["base"]` projections so
  // the base method never borrows the DERIVED place.
  llvm::SmallVector<clang::QualType, 2> baseHops;
  const clang::Expr *receiverExpr =
      peelDerivedToBaseCasts(call->getArg(0), baseHops);
  for (clang::QualType hop : baseHops) {
    const clang::CXXRecordDecl *hopRecord = hop->getAsCXXRecordDecl();
    if (hopRecord && hopRecord->hasDefinition() && hopRecord->isEmpty())
      return emitError(loc)
             << "unsupported: inherited member of an empty base class";
  }
  const clang::Expr *receiverStripped = receiverExpr->IgnoreParenImpCasts();
  // W2.19a mirror: operator syntax normally spells a VALUE receiver, where
  // the static bind is exact C++ semantics, but a dereference spelling
  // (`*p == x`) still names a pointee whose dynamic type is an assumption,
  // and no devirtualization gate runs on this path — a virtual operator
  // through any pointer-shaped receiver keeps the fence.
  if (method->isVirtual()) {
    const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(receiverStripped);
    if ((unary && unary->getOpcode() == clang::UO_Deref) ||
        isDataPointer(receiverStripped->getType()))
      return emitError(loc)
             << "unsupported: virtual method call through a pointer";
  }
  FailureOr<Value> receiver = emitLValue(receiverStripped);
  if (failed(receiver))
    return failure();
  auto receiverLValueType =
      llvm::dyn_cast<emitrust::LValueType>((*receiver).getType());
  if (!receiverLValueType ||
      !llvm::isa<emitrust::StructType>(receiverLValueType.getValueType()))
    return emitError(loc)
           << "unsupported: member call receiver is not a struct place";
  if (!baseHops.empty()) {
    FailureOr<Value> baseReceiver = projectBaseHops(*receiver, baseHops, loc);
    if (failed(baseReceiver))
      return failure();
    receiver = baseReceiver;
  }
  Value addrOf = builder
                     .create<emitrust::AddrOfOp>(loc, targetType.getInput(0),
                                                 *receiver,
                                                 /*is_mut=*/!method->isConst())
                     .getResult();
  // FR-48 mirror: the receiver is one of the borrows the call holds at
  // once, so `a.op(a)` shapes (`a == a` here) are checked exactly like
  // `a.m(a)` — two borrows of one object are sound only if both are
  // shared.
  const clang::Expr *receiverIdExpr = call->getArg(0)->IgnoreParenImpCasts();
  const clang::VarDecl *receiverRoot = placeExprRoot(receiverIdExpr);
  bool receiverIsThis = rootsAtCxxThis(receiverIdExpr);
  bool receiverIsMut = !method->isConst();
  SmallVector<Value> arguments(targetType.getNumInputs(), Value());
  arguments[0] = addrOf;
  SmallVector<std::pair<const clang::VarDecl *, bool>, 4> heldBorrows;
  bool borrowedThis = receiverIsThis;
  bool borrowedThisIsMut = receiverIsThis && receiverIsMut;
  if (receiverRoot)
    heldBorrows.push_back({receiverRoot, receiverIsMut});
  for (unsigned index = 1; index < call->getNumArgs(); ++index) {
    const clang::Expr *argExpr = call->getArg(index);
    Type input = targetType.getInput(index);
    if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(input)) {
      bool argIsMut = llvm::isa<emitrust::MutRefType>(input);
      const clang::VarDecl *argRoot = placeExprRoot(argExpr);
      bool argIsThis = rootsAtCxxThis(argExpr);
      bool collides =
          argIsThis && borrowedThis && (argIsMut || borrowedThisIsMut);
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
      arguments[index] = *reference;
      continue;
    }
    FailureOr<Value> value = emitRValue(argExpr);
    if (failed(value))
      return failure();
    arguments[index] = *value;
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
CImporter::emitStlVectorEndPlace(Value receiver,
                                 emitrust::OpaqueType vectorType, bool isFront,
                                 Location loc) {
  llvm::StringRef spelling = vectorType.getValue();
  // Strip the "Vec<" prefix and trailing ">".
  Type elementType =
      parseStlElementType(spelling.substr(4, spelling.size() - 5));
  if (!elementType)
    return emitError(loc) << "unsupported: " << (isFront ? "front" : "back")
                          << " element type";
  Value index;
  if (isFront) {
    index = builder.create<arith::ConstantOp>(loc, builder.getIndexAttr(0))
                .getResult();
  } else {
    Value len = builder
                    .create<emitrust::MethodCallOp>(
                        loc, TypeRange{builder.getIndexType()}, receiver,
                        builder.getStringAttr("len"), ValueRange{})
                    .getResult(0);
    Value one = builder.create<arith::ConstantOp>(loc, builder.getIndexAttr(1))
                    .getResult();
    index = builder.create<arith::SubIOp>(loc, len, one).getResult();
  }
  return builder
      .create<emitrust::SubscriptOp>(
          loc, emitrust::LValueType::get(elementType), receiver, index)
      .getResult();
}

/// W2.20: peels the wrappers clang puts between an `operator!=` argument
/// and the member call underneath it — the materialized temporary for the
/// by-const-reference iterator parameter, plus the parens/implicit casts.
static const clang::Expr *peelStlIteratorOperand(const clang::Expr *e) {
  e = e->IgnoreParenImpCasts();
  while (const auto *materialize =
             llvm::dyn_cast<clang::MaterializeTemporaryExpr>(e))
    e = materialize->getSubExpr()->IgnoreParenImpCasts();
  return e;
}

/// W2.20: the `m.find(k) != m.end()` / `m.find(k) == m.end()` idiom — the
/// ONE iterator shape the wave admits, and the only one that lowers
/// without materializing an iterator at all. Matches a free
/// `operator==`/`operator!=` whose two operands are, after peeling, member
/// calls named `find` and `end` on the SAME bare local, and hands back the
/// find call. Recognized BEFORE the operands' types are ever mapped: the
/// iterator type would reject first otherwise.
static const clang::CXXMemberCallExpr *
matchStlFindEndIdiom(const clang::CXXOperatorCallExpr *call) {
  if (call->getOperator() != clang::OO_ExclaimEqual &&
      call->getOperator() != clang::OO_EqualEqual)
    return nullptr;
  if (call->getNumArgs() != 2)
    return nullptr;
  const auto *lhs = llvm::dyn_cast<clang::CXXMemberCallExpr>(
      peelStlIteratorOperand(call->getArg(0)));
  const auto *rhs = llvm::dyn_cast<clang::CXXMemberCallExpr>(
      peelStlIteratorOperand(call->getArg(1)));
  if (!lhs || !rhs)
    return nullptr;
  auto memberName = [](const clang::CXXMemberCallExpr *member) {
    const clang::CXXMethodDecl *method = member->getMethodDecl();
    return method && method->getDeclName().isIdentifier()
               ? method->getName()
               : llvm::StringRef();
  };
  const clang::CXXMemberCallExpr *findCall = nullptr;
  const clang::CXXMemberCallExpr *endCall = nullptr;
  if (memberName(lhs) == "find" && memberName(rhs) == "end") {
    findCall = lhs;
    endCall = rhs;
  } else if (memberName(lhs) == "end" && memberName(rhs) == "find") {
    findCall = rhs;
    endCall = lhs;
  } else {
    return nullptr;
  }
  if (findCall->getNumArgs() != 1 || endCall->getNumArgs() != 0)
    return nullptr;
  const clang::CXXMethodDecl *method = findCall->getMethodDecl();
  if (!method || !method->getParent()->isInStdNamespace() ||
      !method->getParent()->getIdentifier())
    return nullptr;
  llvm::StringRef container = method->getParent()->getName();
  if (container != "map" && container != "set")
    return nullptr;
  // Both calls must name the SAME container object, spelled as a bare
  // local both times — anything else (two different maps, a temporary)
  // is not the idiom.
  const auto *findRef = llvm::dyn_cast<clang::DeclRefExpr>(
      findCall->getImplicitObjectArgument()->IgnoreParenImpCasts());
  const auto *endRef = llvm::dyn_cast<clang::DeclRefExpr>(
      endCall->getImplicitObjectArgument()->IgnoreParenImpCasts());
  if (!findRef || !endRef || findRef->getDecl() != endRef->getDecl())
    return nullptr;
  return findCall;
}

FailureOr<Value>
CImporter::emitStlFindEndTest(const clang::CXXOperatorCallExpr *call,
                              const clang::CXXMemberCallExpr *findCall) {
  Location loc = translateLoc(call->getBeginLoc());
  FailureOr<Value> receiver = emitLValue(
      findCall->getImplicitObjectArgument()->IgnoreParenImpCasts());
  if (failed(receiver))
    return failure();
  auto receiverLValueType =
      llvm::dyn_cast<emitrust::LValueType>((*receiver).getType());
  auto opaque = receiverLValueType
                    ? llvm::dyn_cast<emitrust::OpaqueType>(
                          receiverLValueType.getValueType())
                    : emitrust::OpaqueType();
  bool isMap = opaque && isStlMapOpaque(opaque);
  bool isSet = opaque && isStlSetOpaque(opaque);
  if (!isMap && !isSet)
    return emitError(loc)
           << "unsupported: find() receiver is not a recognized STL type";
  Type keyType;
  Type valueType;
  if (isMap) {
    if (!stlMapKeyValueTypes(opaque, keyType, valueType))
      return emitError(loc) << "unsupported: std::map element type";
  } else {
    keyType = stlSetElementType(opaque);
    if (!keyType)
      return emitError(loc) << "unsupported: std::set element type";
  }
  FailureOr<Value> keyRef = emitStlKeyRef(findCall->getArg(0), keyType, loc);
  if (failed(keyRef))
    return failure();
  Value found =
      builder
          .create<emitrust::MethodCallOp>(
              loc, TypeRange{builder.getI1Type()}, *receiver,
              builder.getStringAttr(isMap ? "contains_key" : "contains"),
              ValueRange{*keyRef})
          .getResult(0);
  // `find(k) == end()` is "absent", the negation of `contains_key`.
  if (call->getOperator() == clang::OO_EqualEqual) {
    Value truth = createBoolConstant(loc, true);
    found = builder.create<arith::XOrIOp>(loc, found, truth).getResult();
  }
  return extendBool(loc, found, call->getType());
}

FailureOr<Value> CImporter::emitStlKeyValue(const clang::Expr *keyExpr,
                                           Type keyType, Location loc) {
  FailureOr<Value> key = emitRValue(keyExpr);
  if (failed(key))
    return failure();
  if ((*key).getType() == keyType)
    return *key;
  auto intType = llvm::dyn_cast<IntegerType>(keyType);
  if (!intType || !llvm::isa<IntegerType>((*key).getType()))
    return emitError(loc)
           << "unsupported: std::map/std::set key argument type";
  return castToIntType(loc, *key, intType);
}

FailureOr<Value> CImporter::emitStlKeyRef(const clang::Expr *keyExpr,
                                          Type keyType, Location loc) {
  // Rust's `contains_key`/`contains`/`remove` and `std::ops::Index::index`
  // all take the key BY REFERENCE, and `&<temporary>` has no place to
  // borrow from in this IR — so the key is materialized into its own
  // local first and the shared reference is taken from that place.
  FailureOr<Value> key = emitStlKeyValue(keyExpr, keyType, loc);
  if (failed(key))
    return failure();
  Value cell = createVariablePlace(loc, keyType);
  if (failed(storeToPlace(loc, cell, *key)))
    return failure();
  return builder
      .create<emitrust::AddrOfOp>(loc, emitrust::RefType::get(keyType), cell,
                                  /*is_mut=*/false)
      .getResult();
}

FailureOr<Value>
CImporter::emitStlMapEntryPlace(Value receiver, emitrust::OpaqueType mapType,
                                const clang::Expr *keyExpr, Location loc) {
  // W2.20: THE map place. C++'s `m[k]` on a MISSING key DEFAULT-INSERTS
  // and returns a reference to the new element, so `m[k]` is a MUTATION
  // even in a read position (`int miss = m[99];` inserts 99 and
  // `m.size()` observes it — measured on clang++ and g++ 2026-08-21).
  // Rust's `Index` PANICS on a missing key, so a plain subscript place
  // would be a miscompile in exactly that case. `*m.entry(k).or_default()`
  // reproduces all FOUR spellings — read, write, compound, missing-key
  // read — with one place.
  //
  // The UFCS free-call spelling is deliberate: `emitrust.method_call`
  // renders `place.method(args)` and requires an LVALUE receiver, so
  // `m.entry(k).or_default()` is not expressible as method_calls at all;
  // `Entry::or_default(BTreeMap::entry(&mut m, k))` is two plain
  // `emitrust.call_opaque` ops feeding an `emitrust.deref`.
  Type keyType;
  Type valueType;
  if (!stlMapKeyValueTypes(mapType, keyType, valueType))
    return emitError(loc) << "unsupported: std::map element type";
  std::optional<std::string> keySpelling = rustSpellingForElementType(keyType);
  std::optional<std::string> valueSpelling =
      rustSpellingForElementType(valueType);
  if (!keySpelling || !valueSpelling)
    return emitError(loc) << "unsupported: std::map element type";
  FailureOr<Value> key = emitStlKeyValue(keyExpr, keyType, loc);
  if (failed(key))
    return failure();
  Value mutRef =
      builder
          .create<emitrust::AddrOfOp>(
              loc, emitrust::MutRefType::get(mapType), receiver, /*is_mut=*/true)
          .getResult();
  auto entryType = emitrust::OpaqueType::get(
      builder.getContext(), "std::collections::btree_map::Entry<'_, " +
                                *keySpelling + ", " + *valueSpelling + ">");
  Value entry =
      builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{entryType},
              builder.getStringAttr("std::collections::BTreeMap::entry"),
              /*args=*/ArrayAttr(), ValueRange{mutRef, *key})
          .getResult(0);
  Value slot =
      builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{emitrust::MutRefType::get(valueType)},
              builder.getStringAttr(
                  "std::collections::btree_map::Entry::or_default"),
              /*args=*/ArrayAttr(), ValueRange{entry})
          .getResult(0);
  return builder
      .create<emitrust::DerefOp>(loc, emitrust::LValueType::get(valueType),
                                 slot)
      .getResult();
}

FailureOr<Value>
CImporter::emitStlMapIndexPlace(Value receiver, emitrust::OpaqueType mapType,
                                const clang::Expr *keyExpr, Location loc) {
  // W2.20: `m.at(k)` -> `*std::ops::Index::index(&m, &k)`. C++'s at()
  // THROWS std::out_of_range on a missing key; exceptions are out of the
  // subset, so Rust's panic is a safe refinement — the identical argument
  // W2.6 used for vector front()/back() on an empty vector. Unlike the
  // entry place above this one is a SHARED reference and never inserts,
  // which is exactly at()'s contract; it is read-only, and the three
  // write positions reject a store through it (`rejectStlMapAtWrite`)
  // rather than let it reach rustc as a deferred E0594.
  Type keyType;
  Type valueType;
  if (!stlMapKeyValueTypes(mapType, keyType, valueType))
    return emitError(loc) << "unsupported: std::map element type";
  Value mapRef =
      builder
          .create<emitrust::AddrOfOp>(loc, emitrust::RefType::get(mapType),
                                      receiver, /*is_mut=*/false)
          .getResult();
  FailureOr<Value> keyRef = emitStlKeyRef(keyExpr, keyType, loc);
  if (failed(keyRef))
    return failure();
  Value slot = builder
                   .create<emitrust::CallOpaqueOp>(
                       loc, TypeRange{emitrust::RefType::get(valueType)},
                       builder.getStringAttr("std::ops::Index::index"),
                       /*args=*/ArrayAttr(), ValueRange{mapRef, *keyRef})
                   .getResult(0);
  return builder
      .create<emitrust::DerefOp>(loc, emitrust::LValueType::get(valueType),
                                 slot)
      .getResult();
}

//===----------------------------------------------------------------------===//
// W2.21: std::unique_ptr -> Box<T>
//===----------------------------------------------------------------------===//

llvm::StringRef
CImporter::matchStlBoxRawPointerCall(const clang::Expr *expr) {
  const auto *call =
      llvm::dyn_cast<clang::CXXMemberCallExpr>(expr->IgnoreParenImpCasts());
  if (!call)
    return llvm::StringRef();
  const clang::CXXMethodDecl *method = call->getMethodDecl();
  if (!method || !method->getParent()->isInStdNamespace() ||
      !method->getDeclName().isIdentifier())
    return llvm::StringRef();
  llvm::StringRef name = method->getName();
  if (name != "get" && name != "release")
    return llvm::StringRef();
  if (!isStdUniquePtrRecordType(
          call->getImplicitObjectArgument()->getType()))
    return llvm::StringRef();
  return name;
}

const clang::Expr *CImporter::matchStlBoxDerefBase(const clang::Expr *expr) {
  const auto *opCall =
      llvm::dyn_cast<clang::CXXOperatorCallExpr>(expr->IgnoreParenImpCasts());
  if (!opCall || opCall->getNumArgs() != 1)
    return nullptr;
  if (opCall->getOperator() != clang::OO_Star &&
      opCall->getOperator() != clang::OO_Arrow)
    return nullptr;
  const auto *method =
      llvm::dyn_cast_or_null<clang::CXXMethodDecl>(opCall->getDirectCallee());
  if (!method || !method->getParent()->isInStdNamespace())
    return nullptr;
  const clang::Expr *base = opCall->getArg(0)->IgnoreParenImpCasts();
  if (!isStdUniquePtrRecordType(base->getType()))
    return nullptr;
  return base;
}

bool CImporter::isStlBoxWriteExpr(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParens();
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e);
      member && member->isArrow())
    return matchStlBoxDerefBase(member->getBase()) != nullptr;
  return matchStlBoxDerefBase(e) != nullptr;
}

FailureOr<Value> CImporter::emitStlBoxDerefRef(Value receiver,
                                               emitrust::OpaqueType boxType,
                                               bool wantMut, Location loc) {
  // W2.21: the UFCS free-call spelling is deliberate and mirrors
  // `emitStlMapEntryPlace`: `emitrust.method_call` renders
  // `place.method(args)` and a Box's `Deref` is a TRAIT method, so
  // `std::ops::Deref::deref(&p)` is one `emitrust.call_opaque` over one
  // `emitrust.addr_of`. Rust's own auto-deref would render `*p` directly,
  // but `emitrust.deref` only accepts a ref/mut_ref operand
  // (EmitRustOps.td) and `emitrust.member` only a struct lvalue
  // (EmitRustOps.cpp `MemberOp::verify`), so the borrow is what turns the
  // opaque into a place either op will take.
  Type payload = stlBoxPayloadType(boxType);
  if (!payload)
    return emitError(loc) << "unsupported: std::unique_ptr payload type";
  Type refType = wantMut ? Type(emitrust::MutRefType::get(boxType))
                         : Type(emitrust::RefType::get(boxType));
  Value borrow = builder
                     .create<emitrust::AddrOfOp>(loc, refType, receiver,
                                                 /*is_mut=*/wantMut)
                     .getResult();
  Type payloadRef = wantMut ? Type(emitrust::MutRefType::get(payload))
                            : Type(emitrust::RefType::get(payload));
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{payloadRef},
          builder.getStringAttr(wantMut ? "std::ops::DerefMut::deref_mut"
                                        : "std::ops::Deref::deref"),
          /*args=*/ArrayAttr(), ValueRange{borrow})
      .getResult(0);
}

FailureOr<Value> CImporter::emitStlBoxDerefPlace(Value receiver,
                                                 emitrust::OpaqueType boxType,
                                                 bool wantMut, Location loc) {
  FailureOr<Value> borrow =
      emitStlBoxDerefRef(receiver, boxType, wantMut, loc);
  if (failed(borrow))
    return failure();
  Type payload = stlBoxPayloadType(boxType);
  return builder
      .create<emitrust::DerefOp>(loc, emitrust::LValueType::get(payload),
                                 *borrow)
      .getResult();
}

FailureOr<Value>
CImporter::emitStlBoxMethodCall(const clang::CXXMemberCallExpr *call,
                                const clang::Expr *boxBase, Location loc) {
  const clang::CXXMethodDecl *method = call->getMethodDecl();
  FailureOr<Value> receiver = emitLValue(boxBase);
  if (failed(receiver))
    return failure();
  auto lvalueType =
      llvm::dyn_cast<emitrust::LValueType>((*receiver).getType());
  auto boxType = lvalueType ? llvm::dyn_cast<emitrust::OpaqueType>(
                                  lvalueType.getValueType())
                            : emitrust::OpaqueType();
  if (!boxType || !isStlBoxOpaque(boxType))
    return emitError(loc)
           << "unsupported: member call receiver is not a recognized STL "
              "type";
  std::string name = cxxMethodMangledName(method);
  func::FuncOp target = functions.lookup(name);
  if (!target)
    return emitError(loc) << "unsupported: call to unimported method '"
                          << name << "'";
  FunctionType targetType = target.getFunctionType();
  if (call->getNumArgs() + 1 != targetType.getNumInputs())
    return emitError(loc) << "unsupported: call argument count mismatch";
  // The receiver slot of the imported method must be a plain borrow of the
  // Box's own payload struct: `p.node_bump(..)` is only the same call as
  // `Node::node_bump(&mut *p, ..)` when the two agree.
  Type payload = stlBoxPayloadType(boxType);
  Type selfInput = targetType.getInput(0);
  Type selfPointee;
  if (auto mutRef = llvm::dyn_cast<emitrust::MutRefType>(selfInput))
    selfPointee = mutRef.getPointee();
  else if (auto sharedRef = llvm::dyn_cast<emitrust::RefType>(selfInput))
    selfPointee = sharedRef.getPointee();
  if (!payload || selfPointee != payload)
    return emitError(loc)
           << "unsupported: std::unique_ptr payload does not match the "
              "method receiver";
  SmallVector<Value> arguments;
  for (auto [index, argExpr] : llvm::enumerate(call->arguments())) {
    Type input = targetType.getInput(index + 1);
    // A reference PARAMETER would need a second borrow live across the
    // Box's own auto-deref borrow; out of subset this wave.
    if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(input))
      return emitError(loc)
             << "unsupported: reference argument to a method called through "
                "a std::unique_ptr";
    FailureOr<Value> value = emitRValue(argExpr);
    if (failed(value))
      return failure();
    if ((*value).getType() != input)
      return emitError(loc) << "unsupported: call argument type mismatch";
    arguments.push_back(*value);
  }
  auto callOp = builder.create<emitrust::MethodCallOp>(
      loc, targetType.getResults(), *receiver, builder.getStringAttr(name),
      arguments);
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
}

FailureOr<Value>
CImporter::emitStlMemberCall(const clang::CXXMemberCallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  const clang::CXXMethodDecl *method = call->getMethodDecl();
  std::string methodName = method->getDeclName().isIdentifier()
                                ? method->getName().str()
                                : std::string();
  // W2.12: a `std::string_view` receiver is a decomposed LOCAL (shared
  // literal backing + cursor/len cells; see emitStringViewLocal) with no
  // place of its own, so it is intercepted BEFORE the receiver place
  // emission below (the std::array precedent below handles a non-opaque
  // receiver, but this one has no lvalue at all).
  if (const auto *svRef = llvm::dyn_cast<clang::DeclRefExpr>(
          call->getImplicitObjectArgument()->IgnoreParenImpCasts()))
    if (const auto *svVar = llvm::dyn_cast<clang::VarDecl>(svRef->getDecl()))
      if (stringViewLocals.contains(svVar))
        return emitStringViewMemberCall(call, svVar, loc);
  // The implicit object argument may be wrapped in an implicit
  // qualification-adjustment cast (const-method binding), mirroring
  // `emitCXXMemberCall`.
  FailureOr<Value> receiver = emitLValue(
      call->getImplicitObjectArgument()->IgnoreParenImpCasts());
  if (failed(receiver))
    return failure();
  auto receiverLValueType =
      llvm::dyn_cast<emitrust::LValueType>((*receiver).getType());
  // W2.7: a `std::array<T, N>` receiver maps to `!emitrust.array<NxT>`,
  // not an opaque — its one recognized method is `size()`, a compile-time
  // constant N (emitted index-typed, then cast to the call's declared
  // C type, mirroring emitLenCall's cast convention below).
  if (receiverLValueType) {
    if (auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(
            receiverLValueType.getValueType())) {
      if (methodName == "size" && call->getNumArgs() == 0) {
        Value n = builder
                      .create<arith::ConstantOp>(
                          loc, builder.getIndexAttr(arrayType.getSize()))
                      .getResult();
        FailureOr<Type> resultType = mapType(call->getType(), loc);
        if (failed(resultType))
          return failure();
        auto intType = llvm::dyn_cast<IntegerType>(*resultType);
        if (!intType)
          return emitError(loc) << "unsupported: size result type";
        return builder.create<emitrust::CastOp>(loc, intType, n).getResult();
      }
      return emitError(loc) << "unsupported: std::array::" << methodName
                            << " is not a recognized STL method";
    }
    // W2.14: a std::variant receiver (a synthesized `!emitrust.data_enum`
    // place). Its one recognized method is `index()`, expanded to a
    // RESULT-mode exhaustive match minting the alternative's declaration
    // index as an i32 constant per arm, then cast to the call's declared
    // size_t (mirroring emitLenCall's cast convention). Everything else —
    // valueless_by_exception (exception-machinery state the importer's
    // exception-free subset can never reach), emplace, swap, ... — is a
    // located rejection naming the entity.
    if (auto dataEnum = llvm::dyn_cast<emitrust::DataEnumType>(
            receiverLValueType.getValueType())) {
      if (methodName == "index" && call->getNumArgs() == 0) {
        Value scrutinee = loadPlace(loc, *receiver);
        IntegerType i32Type = builder.getIntegerType(32);
        emitrust::MatchOp match = createVariantMatch(
            loc, scrutinee, dataEnum, i32Type, [&](unsigned index, Value) {
              Value constant = createIntConstant(loc, i32Type, index);
              builder.create<emitrust::YieldOp>(loc, ValueRange{constant});
            });
        FailureOr<Type> resultType = mapType(call->getType(), loc);
        if (failed(resultType))
          return failure();
        auto intType = llvm::dyn_cast<IntegerType>(*resultType);
        if (!intType)
          return emitError(loc) << "unsupported: index result type";
        Value result = match.getResult();
        if (intType != i32Type)
          result =
              builder.create<emitrust::CastOp>(loc, intType, result)
                  .getResult();
        return result;
      }
      return emitError(loc) << "unsupported: std::variant::" << methodName
                            << " is not a recognized STL method";
    }
  }
  auto opaque = receiverLValueType ? llvm::dyn_cast<emitrust::OpaqueType>(
                                         receiverLValueType.getValueType())
                                   : emitrust::OpaqueType();
  if (!opaque || !isStlOpaqueType(opaque))
    return emitError(loc)
           << "unsupported: member call receiver is not a recognized STL "
              "type";
  llvm::StringRef typeSpelling = opaque.getValue();
  bool isVector = typeSpelling.starts_with("Vec<");
  bool isMap = typeSpelling.starts_with("BTreeMap<");
  bool isSet = typeSpelling.starts_with("BTreeSet<");

  // W2.21: a `std::unique_ptr` receiver. The Box image admits NO member
  // vocabulary of its own this wave — the whole access surface is the
  // PAYLOAD's, reached through `operator*`/`operator->` — so every method
  // named here is a located rejection, and the three that would otherwise
  // be tempting get wordings that name the real blocker instead of the
  // generic "not recognized" tail:
  //  * get()/release() hand out a RAW POINTER into the Box. The project's
  //    pointer model scalarizes a raw `T *` to a local away entirely
  //    (measured), so there is no value to hand back, and release()
  //    additionally LEAKS unless the caller captures and frees it.
  //  * reset() (with or without an argument) and `operator bool` are
  //    NULLABILITY: a Rust `Box<T>` cannot be null, which is exactly the
  //    boundary this wave draws (see mapStdLibraryType).
  if (typeSpelling.starts_with("Box<")) {
    if (methodName == "get" || methodName == "release")
      return emitError(loc)
             << "unsupported: std::unique_ptr::" << methodName
             << "() hands out a raw pointer to the payload, which has no "
                "place in this model";
    if (methodName == "reset")
      return emitError(loc)
             << "unsupported: a Box<T> cannot be null, so std::unique_ptr::"
                "reset() has no image";
    if (!method->getDeclName().isIdentifier())
      return emitError(loc)
             << "unsupported: a Box<T> cannot be null, so testing a "
                "std::unique_ptr for emptiness has no image";
    return emitError(loc) << "unsupported: std::unique_ptr::" << methodName
                          << " is not a recognized STL method";
  }

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
    // W2.6: front()/back() lower to index places (`v[0]` /
    // `v[v.len() - 1]`), shared with emitLValue's value-read path.
    if (methodName == "front" || methodName == "back") {
      if (call->getNumArgs() != 0)
        return emitError(loc)
               << "unsupported: " << methodName << " takes no arguments";
      FailureOr<Value> place = emitStlVectorEndPlace(
          *receiver, opaque, /*isFront=*/methodName == "front", loc);
      if (failed(place))
        return failure();
      return loadPlace(loc, *place);
    }
    if (methodName == "pop_back") {
      // `pop_back` on an empty vector is C++ UB; Rust's `pop` is simply a
      // no-op there (returns None) — a benign refinement. The Option result
      // is deliberately unbound (statement position).
      if (call->getNumArgs() != 0)
        return emitError(loc) << "unsupported: pop_back takes no arguments";
      builder.create<emitrust::MethodCallOp>(loc, TypeRange(), *receiver,
                                             builder.getStringAttr("pop"),
                                             ValueRange{});
      return Value();
    }
    return emitError(loc) << "unsupported: std::vector::" << methodName
                          << " is not a recognized STL method";
  }
  // W2.20: std::map / std::set. size()/empty() reuse the family-agnostic
  // lambdas above verbatim. count()/erase() take the key BY REFERENCE and
  // return a COUNT (0 or 1), not a bool — hence the `contains_key` probe
  // in front of the discarded `remove`, which is two lookups but exact.
  // at() is the read-only `Index::index` place. std::set::insert matches
  // Rust's `BTreeSet::insert` EXACTLY (neither replaces an existing
  // element, and both report whether the set changed).
  //
  // std::map::insert is the wave's other miscompile trap and is REJECTED:
  // C++'s `map::insert` does NOT overwrite an existing key while Rust's
  // `BTreeMap::insert` DOES (measured 2026-08-21: after `m[1]=10`,
  // `m.insert({1,99})` leaves m[1]==10 and returns false). Mapping
  // insert -> insert would be silent wrong code.
  if (isMap || isSet) {
    Type keyType;
    Type valueType;
    if (isMap) {
      if (!stlMapKeyValueTypes(opaque, keyType, valueType))
        return emitError(loc) << "unsupported: std::map element type";
    } else {
      keyType = stlSetElementType(opaque);
      if (!keyType)
        return emitError(loc) << "unsupported: std::set element type";
    }
    llvm::StringRef family = isMap ? "std::map" : "std::set";
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
    if (methodName == "count" || methodName == "contains" ||
        methodName == "erase") {
      if (call->getNumArgs() != 1)
        return emitError(loc) << "unsupported: " << methodName
                              << " requires exactly one argument";
      FailureOr<Value> keyRef = emitStlKeyRef(call->getArg(0), keyType, loc);
      if (failed(keyRef))
        return failure();
      Value found =
          builder
              .create<emitrust::MethodCallOp>(
                  loc, TypeRange{builder.getI1Type()}, *receiver,
                  builder.getStringAttr(isMap ? "contains_key" : "contains"),
                  ValueRange{*keyRef})
              .getResult(0);
      if (methodName == "erase") {
        // The removal's own result is DISCARDED: C++ erase(key) reports
        // the count, which the probe above already holds. Giving the op a
        // result (typed `Option<V>` for a map, `bool` for a set) rather
        // than none keeps the emitter's `_vN` naming, which absorbs
        // `Option`'s #[must_use].
        std::optional<std::string> valueSpelling =
            isMap ? rustSpellingForElementType(valueType) : std::nullopt;
        if (isMap && !valueSpelling)
          return emitError(loc) << "unsupported: std::map element type";
        Type removeResult =
            isMap ? Type(emitrust::OpaqueType::get(builder.getContext(),
                                                   "Option<" + *valueSpelling +
                                                       ">"))
                  : Type(builder.getI1Type());
        builder.create<emitrust::MethodCallOp>(
            loc, TypeRange{removeResult}, *receiver,
            builder.getStringAttr("remove"), ValueRange{*keyRef});
      }
      FailureOr<Type> resultType = mapType(call->getType(), loc);
      if (failed(resultType))
        return failure();
      if (llvm::isa<IntegerType>(*resultType) &&
          *resultType != builder.getI1Type())
        return builder.create<emitrust::CastOp>(loc, *resultType, found)
            .getResult();
      if (*resultType == builder.getI1Type())
        return found;
      return emitError(loc) << "unsupported: " << methodName
                            << " result type";
    }
    if (isMap && methodName == "at") {
      if (call->getNumArgs() != 1)
        return emitError(loc)
               << "unsupported: at requires exactly one argument";
      FailureOr<Value> place =
          emitStlMapIndexPlace(*receiver, opaque, call->getArg(0), loc);
      if (failed(place))
        return failure();
      return loadPlace(loc, *place);
    }
    if (isSet && methodName == "insert") {
      if (call->getNumArgs() != 1)
        return emitError(loc)
               << "unsupported: insert requires exactly one argument";
      FailureOr<Value> element =
          emitStlKeyValue(call->getArg(0), keyType, loc);
      if (failed(element))
        return failure();
      builder.create<emitrust::MethodCallOp>(loc, TypeRange(), *receiver,
                                             builder.getStringAttr("insert"),
                                             ValueRange{*element});
      return Value();
    }
    if (isMap && (methodName == "insert" || methodName == "emplace" ||
                  methodName == "try_emplace"))
      return emitError(loc)
             << "unsupported: std::map::" << methodName
             << " does not overwrite an existing key while Rust's "
                "BTreeMap::insert does";
    if (methodName == "find" || methodName == "begin" || methodName == "end" ||
        methodName == "cbegin" || methodName == "cend" ||
        methodName == "rbegin" || methodName == "rend" ||
        methodName == "crbegin" || methodName == "crend" ||
        methodName == "lower_bound" || methodName == "upper_bound" ||
        methodName == "equal_range" || methodName == "emplace_hint")
      return emitError(loc) << "unsupported: std::map/std::set iterators are "
                               "only recognized in the find(k) != end() "
                               "idiom";
    return emitError(loc) << "unsupported: " << family << "::" << methodName
                          << " is not a recognized STL method";
  }
  // W2.11: std::optional. `has_value()` -> `is_some()` (a genuine bool on
  // both sides); `value_or(d)` -> `unwrap_or(d)` (identical semantics:
  // the contained value if engaged, else the default — both by value).
  // Everything else — value() (panic message differs from the C++
  // exception), operator*/operator-> (UB when empty; no place model for
  // the contained value this wave) — is a located rejection.
  if (typeSpelling.starts_with("Option<")) {
    if (methodName == "has_value") {
      if (call->getNumArgs() != 0)
        return emitError(loc)
               << "unsupported: has_value takes no arguments";
      return builder
          .create<emitrust::MethodCallOp>(loc, TypeRange{builder.getI1Type()},
                                          *receiver,
                                          builder.getStringAttr("is_some"),
                                          ValueRange{})
          .getResult(0);
    }
    if (methodName == "value_or") {
      if (call->getNumArgs() != 1)
        return emitError(loc)
               << "unsupported: value_or requires exactly one argument";
      Type elementType = parseStlElementType(
          typeSpelling.substr(7, typeSpelling.size() - 8));
      FailureOr<Value> argument = emitRValue(call->getArg(0));
      if (failed(argument))
        return failure();
      if (!elementType || (*argument).getType() != elementType)
        return emitError(loc)
               << "unsupported: std::optional::value_or argument type";
      return builder
          .create<emitrust::MethodCallOp>(loc, TypeRange{elementType},
                                          *receiver,
                                          builder.getStringAttr("unwrap_or"),
                                          ValueRange{*argument})
          .getResult(0);
    }
    return emitError(loc) << "unsupported: std::optional::" << methodName
                          << " is not a recognized STL method";
  }
  // std::string.
  if (methodName == "size" || methodName == "length")
    return emitLenCall();
  if (methodName == "empty")
    return emitEmptyCall();
  // W2.6: push_back(c) is the method spelling of `+= 'c'` and reuses its
  // exact emission (`push(c as char)` via the printf-'%c' ASCII policy in
  // wrapCharFormat); clear() mirrors the vector spelling.
  if (methodName == "push_back") {
    if (call->getNumArgs() != 1)
      return emitError(loc)
             << "unsupported: push_back requires exactly one argument";
    if (!call->getArg(0)->getType().getCanonicalType()->isIntegerType())
      return emitError(loc)
             << "unsupported: std::string::push_back argument type";
    FailureOr<Value> argument = emitRValue(call->getArg(0));
    if (failed(argument))
      return failure();
    Value character = wrapCharFormat(loc, *argument);
    builder.create<emitrust::MethodCallOp>(loc, TypeRange(), *receiver,
                                           builder.getStringAttr("push"),
                                           ValueRange{character});
    return Value();
  }
  if (methodName == "clear") {
    if (call->getNumArgs() != 0)
      return emitError(loc) << "unsupported: clear takes no arguments";
    builder.create<emitrust::MethodCallOp>(loc, TypeRange(), *receiver,
                                           builder.getStringAttr("clear"),
                                           ValueRange{});
    return Value();
  }
  if (methodName == "c_str")
    return emitError(loc)
           << "unsupported: std::string::c_str() is only recognized as a "
              "printf '%s' argument";
  return emitError(loc) << "unsupported: std::string::" << methodName
                        << " is not a recognized STL method";
}

FailureOr<Value>
CImporter::emitStringViewMemberCall(const clang::CXXMemberCallExpr *call,
                                    const clang::VarDecl *var, Location loc) {
  // W2.12: the method table of a decomposed string_view local. size()
  // loads the len cell and casts to the call expression's declared C type
  // (the emitLenCall convention); remove_prefix(n) is the cursor += n /
  // len -= n pair (C++ requires n <= size() — UB otherwise — so the plain
  // arithmetic is exact for defined programs; statement position, returns
  // void). Everything else (data, substr, find, remove_suffix, front,
  // back, ...) is a located rejection naming the entity.
  const StringViewLocalInfo &info = stringViewLocals.find(var)->second;
  const clang::CXXMethodDecl *method = call->getMethodDecl();
  std::string methodName = method->getDeclName().isIdentifier()
                                ? method->getName().str()
                                : std::string();
  IntegerType i64Type = builder.getIntegerType(64);
  if (methodName == "size") {
    if (call->getNumArgs() != 0)
      return emitError(loc) << "unsupported: size takes no arguments";
    Value len = loadPlace(loc, info.lenCell);
    FailureOr<Type> resultType = mapType(call->getType(), loc);
    if (failed(resultType))
      return failure();
    auto intType = llvm::dyn_cast<IntegerType>(*resultType);
    if (!intType)
      return emitError(loc) << "unsupported: size result type";
    return castToIntType(loc, len, intType);
  }
  if (methodName == "remove_prefix") {
    if (call->getNumArgs() != 1)
      return emitError(loc)
             << "unsupported: remove_prefix requires exactly one argument";
    FailureOr<Value> amount = emitRValue(call->getArg(0));
    if (failed(amount))
      return failure();
    if (!llvm::isa<IntegerType>((*amount).getType()))
      return emitError(loc)
             << "unsupported: remove_prefix amount must be an integer";
    Value amount64 = castToIntType(loc, *amount, i64Type);
    Value cursor = loadPlace(loc, info.cursorCell);
    Value advanced =
        builder.create<arith::AddIOp>(loc, cursor, amount64).getResult();
    builder.create<memref::StoreOp>(loc, advanced, info.cursorCell);
    Value len = loadPlace(loc, info.lenCell);
    Value trimmed =
        builder.create<arith::SubIOp>(loc, len, amount64).getResult();
    builder.create<memref::StoreOp>(loc, trimmed, info.lenCell);
    return Value();
  }
  return emitError(loc) << "unsupported: std::string_view::" << methodName
                        << " is not a recognized STL method";
}

FailureOr<Value>
CImporter::emitStringViewIndexPlace(const clang::VarDecl *var,
                                    const clang::Expr *idxExpr, Location loc) {
  // W2.12: `sv[i]` — the shared literal backing subscripted at
  // cursor + i, an `!emitrust.lvalue<i8>` byte place (C `char`
  // semantics). Read position only: the backing is const, and C++
  // requires i < size() (UB otherwise), so Rust's bounds panic on an
  // out-of-range index is a safe refinement.
  const StringViewLocalInfo &info = stringViewLocals.find(var)->second;
  FailureOr<Value> index = emitRValue(idxExpr);
  if (failed(index))
    return failure();
  if (!llvm::isa<IntegerType>((*index).getType()))
    return emitError(loc)
           << "unsupported: operator[] index must be an integer";
  Value index64 = castToIntType(loc, *index, builder.getIntegerType(64));
  Value cursor = loadPlace(loc, info.cursorCell);
  Value position =
      builder.create<arith::AddIOp>(loc, cursor, index64).getResult();
  return builder
      .create<emitrust::SubscriptOp>(
          loc, emitrust::LValueType::get(builder.getIntegerType(8)),
          info.backing, position)
      .getResult();
}

FailureOr<Value>
CImporter::emitStlOperatorCall(const clang::CXXOperatorCallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  // W2.12: a `std::string_view` receiver — a decomposed local with no
  // place of its own — is intercepted before the receiver place emission
  // below. `sv[i]` loads the backing[cursor + i] byte place; every other
  // operator (operator= rebinding after init included) is a located
  // rejection naming the entity.
  if (const auto *svRef = llvm::dyn_cast<clang::DeclRefExpr>(
          call->getArg(0)->IgnoreParenImpCasts()))
    if (const auto *svVar = llvm::dyn_cast<clang::VarDecl>(svRef->getDecl()))
      if (stringViewLocals.contains(svVar)) {
        if (call->getOperator() == clang::OO_Subscript) {
          if (call->getNumArgs() != 2)
            return emitError(loc)
                   << "unsupported: operator[] requires exactly one index "
                      "argument";
          FailureOr<Value> place =
              emitStringViewIndexPlace(svVar, call->getArg(1), loc);
          if (failed(place))
            return failure();
          return loadPlace(loc, *place);
        }
        return emitError(loc)
               << "unsupported: std::string_view::operator"
               << clang::getOperatorSpelling(call->getOperator())
               << " is not a recognized STL method";
      }
  FailureOr<Value> receiver =
      emitLValue(call->getArg(0)->IgnoreParenImpCasts());
  if (failed(receiver))
    return failure();
  auto receiverLValueType =
      llvm::dyn_cast<emitrust::LValueType>((*receiver).getType());
  // W2.14: a std::variant receiver (a synthesized `!emitrust.data_enum`
  // place). operator= from an alternative VALUE re-tags the place with a
  // fresh enum_variant — assignment replaces the held alternative
  // wholesale on both sides, so no in-place mutation image is needed;
  // the alternative is selected by EXACT mapped-type equality, like the
  // converting ctor. Every other operator (==, <, ...) and every other
  // right-hand shape (another variant, a non-alternative value) is a
  // located rejection naming the entity.
  if (receiverLValueType) {
    if (auto dataEnum = llvm::dyn_cast<emitrust::DataEnumType>(
            receiverLValueType.getValueType())) {
      if (call->getOperator() != clang::OO_Equal)
        return emitError(loc)
               << "unsupported: std::variant::operator"
               << clang::getOperatorSpelling(call->getOperator())
               << " is not a recognized STL method";
      if (call->getNumArgs() != 2)
        return emitError(loc)
               << "unsupported: operator= requires exactly one argument";
      FailureOr<Value> rhs = emitRValue(call->getArg(1));
      if (failed(rhs))
        return failure();
      std::optional<unsigned> index =
          variantAltIndex(dataEnum, (*rhs).getType());
      if (!index)
        return emitError(loc)
               << "unsupported: std::variant::operator= right-hand side "
                  "type";
      Value value = createVariantValue(loc, dataEnum, *index, *rhs);
      if (failed(storeToPlace(loc, *receiver, value)))
        return failure();
      return Value();
    }
  }
  auto opaque = receiverLValueType ? llvm::dyn_cast<emitrust::OpaqueType>(
                                         receiverLValueType.getValueType())
                                   : emitrust::OpaqueType();
  if (!opaque || !isStlOpaqueType(opaque))
    return emitError(loc)
           << "unsupported: operator call receiver is not a recognized STL "
              "type";
  bool isVector = opaque.getValue().starts_with("Vec<");

  // W2.21: a `std::unique_ptr` receiver. `*p` and `p->` are the WHOLE
  // admitted operator surface; both resolve to the payload through the
  // Deref borrow, and `p->x` hands the BORROW back (not a place) because
  // `emitMemberBasePlace`'s arrow branch expects a ref/mut_ref and builds
  // the `emitrust.deref` itself, exactly as it does for a `&mut S`
  // parameter. Assignment and equality are the NULLABILITY / MOVE
  // boundaries and name themselves.
  if (isStlBoxOpaque(opaque)) {
    if (call->getOperator() == clang::OO_Star) {
      FailureOr<Value> place =
          emitStlBoxDerefPlace(*receiver, opaque, stlBoxWriteContext, loc);
      if (failed(place))
        return failure();
      return loadPlace(loc, *place);
    }
    if (call->getOperator() == clang::OO_Arrow)
      return emitStlBoxDerefRef(*receiver, opaque, stlBoxWriteContext, loc);
    if (call->getOperator() == clang::OO_Equal && call->getNumArgs() == 2) {
      const clang::Expr *rhs = call->getArg(1)->IgnoreParenImpCasts();
      if (isStdUniquePtrRecordType(rhs->getType()))
        return emitError(loc)
               << "unsupported: a moved-from std::unique_ptr is null and "
                  "testable, but Rust cannot read a moved-from binding";
      return emitError(loc)
             << "unsupported: a Box<T> cannot be null, so assigning nullptr "
                "to a std::unique_ptr has no image";
    }
    return emitError(loc) << "unsupported: std::unique_ptr::operator"
                          << clang::getOperatorSpelling(call->getOperator())
                          << " is not a recognized STL method";
  }

  switch (call->getOperator()) {
  case clang::OO_Subscript: {
    if (call->getNumArgs() != 2)
      return emitError(loc)
             << "unsupported: operator[] requires exactly one index "
                "argument";
    // W2.20: a std::map receiver takes the DEFAULT-INSERTING entry place,
    // never `emitrust.subscript` (which renders `m[i as usize]`). This
    // read-position spelling is a MUTATION in C++ too, so it shares the
    // one place with the write and compound spellings.
    if (isStlMapOpaque(opaque)) {
      FailureOr<Value> place =
          emitStlMapEntryPlace(*receiver, opaque, call->getArg(1), loc);
      if (failed(place))
        return failure();
      return loadPlace(loc, *place);
    }
    if (!isVector) {
      // W2.20: this fall-through used to hardcode "std::string" for EVERY
      // non-Vec opaque, which becomes actively misleading now that a map
      // or set receiver can reach it. The std::string wording is kept
      // verbatim for a genuine std::string receiver (it names the real
      // reason — byte indexing).
      if (opaque.getValue() == "String")
        return emitError(loc)
               << "unsupported: std::string::operator[] is not a recognized "
                  "STL method (bytes indexing is not supported this wave)";
      return emitError(loc)
             << "unsupported: " << stlOpaqueDisplayName(opaque)
             << "::operator[] is not a recognized STL method";
    }
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
    // W2.20: same rewording as operator[] above — a map/set receiver must
    // not be reported as a std::string.
    if (isStlMapOpaque(opaque) || isStlSetOpaque(opaque))
      return emitError(loc)
             << "unsupported: " << stlOpaqueDisplayName(opaque)
             << "::operator+= is not a recognized STL method";
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

emitrust::MatchOp CImporter::createVariantMatch(
    Location loc, Value scrutinee, emitrust::DataEnumType enumType,
    Type resultType,
    llvm::function_ref<void(unsigned index, Value payload)> buildArm) {
  llvm::SmallVector<Type, 2> alternatives =
      variantEnumAlternatives.lookup(enumType.getName());
  auto match = builder.create<emitrust::MatchOp>(
      loc, resultType ? TypeRange{resultType} : TypeRange{}, scrutinee,
      builder.getStrArrayAttr({"V0", "V1"}), /*caseRegionsCount=*/2);
  OpBuilder::InsertionGuard guard(builder);
  for (unsigned index = 0; index < 2; ++index) {
    Block &block = match.getCaseRegions()[index].emplaceBlock();
    Value payload = block.addArgument(alternatives[index], loc);
    builder.setInsertionPointToStart(&block);
    buildArm(index, payload);
  }
  return match;
}

const clang::CallExpr *CImporter::matchVariantGetCall(const clang::Expr *e) {
  const auto *call = llvm::dyn_cast<clang::CallExpr>(e->IgnoreParens());
  if (!call || llvm::isa<clang::CXXMemberCallExpr>(call) ||
      call->getNumArgs() != 1)
    return nullptr;
  const clang::FunctionDecl *callee = call->getDirectCallee();
  if (!callee || !callee->isInStdNamespace() ||
      !callee->getDeclName().isIdentifier() || callee->getName() != "get")
    return nullptr;
  if (!isStdVariantRecordType(call->getArg(0)->getType()))
    return nullptr;
  return call;
}

FailureOr<Value> CImporter::emitVariantGet(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  const clang::FunctionDecl *callee = call->getDirectCallee();
  const clang::TemplateArgumentList *templateArgs =
      callee->getTemplateSpecializationArgs();
  if (!templateArgs || templateArgs->size() < 1)
    return emitError(loc)
           << "unsupported: std::get shape could not be determined";
  const clang::TemplateArgument &selector = templateArgs->get(0);
  if (selector.getKind() != clang::TemplateArgument::Type)
    return emitError(loc)
           << "unsupported: std::get<index> over a std::variant (only the "
              "alternative-type form std::get<T> is recognized)";
  FailureOr<Value> receiver =
      emitLValue(call->getArg(0)->IgnoreParenImpCasts());
  if (failed(receiver))
    return failure();
  auto receiverLValueType =
      llvm::dyn_cast<emitrust::LValueType>((*receiver).getType());
  auto enumType = receiverLValueType
                      ? llvm::dyn_cast<emitrust::DataEnumType>(
                            receiverLValueType.getValueType())
                      : emitrust::DataEnumType();
  if (!enumType)
    return emitError(loc) << "unsupported: std::get receiver is not a "
                             "recognized std::variant local";
  FailureOr<Type> requested = mapType(selector.getAsType(), loc);
  if (failed(requested))
    return failure();
  std::optional<unsigned> held = variantAltIndex(enumType, *requested);
  if (!held)
    return emitError(loc) << "unsupported: std::get type argument is not "
                             "an alternative of this std::variant";
  // The held arm yields its payload; the OTHER arm diverges through the
  // panic! image (already in the emitter's diverging set, so the
  // mandatory RESULT-mode yield after it — carrying a dummy typed
  // operand the emitter provably drops — never renders). C++ throws
  // bad_variant_access here; catch is unsupported, so no program in the
  // supported subset can observe the difference.
  emitrust::MatchOp match = createVariantMatch(
      loc, loadPlace(loc, *receiver), enumType, *requested,
      [&](unsigned index, Value payload) {
        if (index == *held) {
          builder.create<emitrust::YieldOp>(loc, ValueRange{payload});
          return;
        }
        Attribute message =
            builder.getStringAttr("std::get: wrong variant alternative");
        builder.create<emitrust::CallOpaqueOp>(
            loc, TypeRange(), builder.getStringAttr("panic!"),
            builder.getArrayAttr({message}), ValueRange());
        Value dummy =
            builder
                .create<arith::ConstantOp>(
                    loc,
                    llvm::cast<TypedAttr>(builder.getZeroAttr(*requested)))
                .getResult();
        builder.create<emitrust::YieldOp>(loc, ValueRange{dummy});
      });
  return match.getResult();
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
  SmallVector<PendingCursor, 2> pairedArgs;
  SmallVector<PendingCursor, 2> globalArgs;
  SmallVector<PendingBorrow, 4> borrows;
  // Value arguments materialize first (C leaves evaluation order
  // unspecified); every borrow-producing argument follows, immediately
  // ahead of the call.
  for (unsigned index = 0; index < numParams; ++index) {
    const clang::Expr *argument = call->getArg(index);
    bool isCursorSlot = cursorParams.contains(definition->getParamDecl(index));
    bool isPairedSlot =
        pairedCursorParams.contains(definition->getParamDecl(index));
    bool isGlobalSlot =
        globalCursorParams.contains(definition->getParamDecl(index));
    if (isCursorSlot || isPairedSlot || isGlobalSlot) {
      // The argument must be `&p` over a decomposed pointer local — the
      // caller-side cursor the callee's advancement (Shape S), unique
      // out-write (Shape P), or Option out-cell (Shape G) lands in.
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
               << "unsupported: a cursor argument must be the "
                  "address of a decomposed pointer local";
      (isCursorSlot   ? cursorArgs
       : isPairedSlot ? pairedArgs
                      : globalArgs)
          .push_back({index, pointer});
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
  SmallVector<const clang::VarDecl *, 4> borrowRoots(numParams, nullptr);
  for (const PendingBorrow &borrow : borrows) {
    const clang::VarDecl *root = nullptr;
    FailureOr<Value> reference = emitBorrowArgument(
        loc, borrow.expr, targetType.getInput(slots[borrow.index]), root);
    if (failed(reference))
      return failure();
    arguments[slots[borrow.index]] = *reference;
    borrowRoots[borrow.index] = root;
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
             << "unsupported: a cursor argument must walk a single "
                "whole region";
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
    // The region's element type must be the callee's declared element
    // (i8 for the historical char** byte cursor; the literal-backing
    // path stays byte-only by construction — a literal region is i8).
    unsigned slotIndex = slots[cursorArg.index];
    auto calleeSlice = llvm::cast<emitrust::SliceType>(
        llvm::cast<emitrust::RefType>(targetType.getInput(slotIndex))
            .getPointee());
    if (elementType != calleeSlice.getElementType())
      return emitError(loc) << "unsupported: a cursor argument must walk "
                               "a region of the parameter's element type";
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
  // Shape-P paired out-cursor arguments (C99-43 slice 1b): pass the
  // address of a staged i64 temp the callee overwrites (initialized to 0
  // only for definite assignment — the admitted write is unconditional),
  // then store co-argument-cursor-at-call + temp into `e`'s cursor cell.
  // The addition is the reslice coordinate correction: ordinary slice
  // arguments RESLICE at the caller's cursor (see emitBorrowArgument's
  // pointer-local path), so the callee's coordinate 0 is the
  // co-argument's cursor, not the region start.
  struct StagedPaired {
    Value tmpPlace;
    Value cell;
    Value coCursor; // Null for a whole-array decay co-argument (zero).
  };
  SmallVector<StagedPaired, 2> stagedPaired;
  for (const PendingCursor &pairedArg : pairedArgs) {
    const clang::ParmVarDecl *coParam =
        pairedCursorParams.lookup(definition->getParamDecl(pairedArg.index));
    unsigned coIndex = numParams;
    for (unsigned candidate = 0; candidate < numParams; ++candidate)
      if (definition->getParamDecl(candidate) == coParam) {
        coIndex = candidate;
        break;
      }
    if (coIndex == numParams || coIndex >= call->getNumArgs())
      return emitError(loc) // Defensive; planning mapped a real sibling.
             << "unsupported: call argument count mismatch";
    // The co-slot must be a slice reference — the coordinate system the
    // writeback below is relative to.
    Type coInput = targetType.getInput(slots[coIndex]);
    Type coPointee;
    if (auto ref = llvm::dyn_cast<emitrust::RefType>(coInput))
      coPointee = ref.getPointee();
    else if (auto mutRef = llvm::dyn_cast<emitrust::MutRefType>(coInput))
      coPointee = mutRef.getPointee();
    if (!coPointee || !llvm::isa<emitrust::SliceType>(coPointee))
      return emitError(loc)
             << "unsupported: a paired cursor argument must walk the "
                "co-argument's region";
    // Resolve the co-argument's root object and its cursor at the call:
    // a direct whole-array decay is coordinate zero; a decomposed
    // single-base non-null pointer local contributes its current cursor.
    const clang::Expr *coArg = stripTrivia(call->getArg(coIndex));
    while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(coArg))
      coArg = stripTrivia(cast->getSubExpr());
    const clang::VarDecl *coRoot = nullptr;
    Value coCursor;
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(coArg))
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
        if (var->getType().getCanonicalType()->isArrayType()) {
          coRoot = var;
        } else if (auto coIt = pointerLocals.find(var);
                   coIt != pointerLocals.end()) {
          const PointerLocalInfo &coInfo = coIt->second;
          if (coInfo.cursorCell && !coInfo.nonNullCell &&
              !coInfo.baseIndexCell && !coInfo.member &&
              !coInfo.literalBacking) {
            coRoot = coInfo.base;
            coCursor = loadPlace(loc, coInfo.cursorCell);
          }
        }
      }
    // `e` must walk that same single region (the writeback stores an
    // absolute cursor over the SHARED base); `emitBorrowArgument`'s own
    // root for the co-slot must agree.
    const PointerLocalInfo &info =
        pointerLocals.find(pairedArg.pointer)->second;
    if (!coRoot || !info.cursorCell || info.nonNullCell ||
        info.baseIndexCell || info.member || info.literalBacking ||
        info.base != coRoot ||
        (borrowRoots[coIndex] && borrowRoots[coIndex] != coRoot))
      return emitError(loc)
             << "unsupported: a paired cursor argument must walk the "
                "co-argument's region";
    unsigned slotIndex = slots[pairedArg.index];
    Value tmpPlace = createVariablePlace(loc, i64Type);
    Value zero = createIntConstant(loc, i64Type, 0);
    builder.create<emitrust::AssignOp>(loc, tmpPlace, zero);
    arguments[slotIndex] =
        builder
            .create<emitrust::AddrOfOp>(loc, targetType.getInput(slotIndex),
                                        tmpPlace, /*is_mut=*/true)
            .getResult();
    stagedPaired.push_back({tmpPlace, info.cursorCell, coCursor});
  }
  // Shape-G single-global-or-NULL out-cell arguments (C99-43 C1): pass
  // the address of a staged `Option<i64>` temp (initialized None only
  // for definite assignment — the admitted write is unconditional),
  // then destructure the written Option back into the pointer local's
  // cells: the CTS-P8 non-null flag takes `.is_some()` and the cursor
  // cell takes `.unwrap_or(0)` (dead while the flag is false; the
  // never-null plan reads its offset back with the Q3 deterministic
  // panic spelling `.expect("null pointer read")` instead). Later uses
  // of the pointer then flow through the decomposed-pointer machinery
  // unchanged, resolving against the plan's statically-known global.
  struct StagedGlobal {
    Value tmpPlace;
    Value cursorCell; // Null for a degenerate (scalar-global) pointer.
    Value flagCell;   // Null when the region never sees NULL.
  };
  SmallVector<StagedGlobal, 2> stagedGlobal;
  for (const PendingCursor &globalArg : globalArgs) {
    const GlobalCursorPlan &plan =
        globalCursorParams.lookup(definition->getParamDecl(globalArg.index));
    const PointerLocalInfo &info =
        pointerLocals.find(globalArg.pointer)->second;
    // The pointer must decompose against exactly the plan's global (a
    // pure-NULL plan constrains nothing): a multi-base, member-rooted,
    // literal- or allocation-backed pointer has no representation for
    // the returned offset.
    if (info.baseIndexCell || info.member || info.literalBacking ||
        info.backing || (plan.global && info.base != plan.global))
      return emitError(loc)
             << "unsupported: a global out-cell argument must decompose "
                "against the callee's target global";
    unsigned slotIndex = slots[globalArg.index];
    Value tmpPlace = createVariablePlace(loc, optionCursorType());
    Value none = builder
                     .create<emitrust::LiteralOp>(loc, optionCursorType(),
                                                  builder.getStringAttr("None"))
                     .getResult();
    builder.create<emitrust::AssignOp>(loc, tmpPlace, none);
    arguments[slotIndex] =
        builder
            .create<emitrust::AddrOfOp>(loc, targetType.getInput(slotIndex),
                                        tmpPlace, /*is_mut=*/true)
            .getResult();
    stagedGlobal.push_back({tmpPlace, info.cursorCell, info.nonNullCell});
  }
  for (auto [index, value] : llvm::enumerate(arguments))
    if (!value || value.getType() != targetType.getInput(index))
      return emitError(loc) << "unsupported: call argument type mismatch";
  auto callOp = builder.create<func::CallOp>(loc, target, arguments);
  for (const StagedCursor &staged : stagedCursors) {
    Value advanced = loadPlace(loc, staged.tmpPlace);
    builder.create<memref::StoreOp>(loc, advanced, staged.cell);
  }
  for (const StagedPaired &staged : stagedPaired) {
    Value written = loadPlace(loc, staged.tmpPlace);
    Value absolute =
        staged.coCursor
            ? builder.create<arith::AddIOp>(loc, staged.coCursor, written)
                  .getResult()
            : written;
    builder.create<memref::StoreOp>(loc, absolute, staged.cell);
  }
  for (const StagedGlobal &staged : stagedGlobal) {
    if (staged.flagCell) {
      Value flag = builder
                       .create<emitrust::MethodCallOp>(
                           loc, TypeRange{builder.getI1Type()},
                           staged.tmpPlace,
                           builder.getStringAttr("is_some"), ValueRange{})
                       .getResult(0);
      builder.create<memref::StoreOp>(loc, flag, staged.flagCell);
    }
    if (staged.cursorCell) {
      Value cursor;
      if (staged.flagCell) {
        Value zero = createIntConstant(loc, i64Type, 0);
        cursor = builder
                     .create<emitrust::MethodCallOp>(
                         loc, TypeRange{i64Type}, staged.tmpPlace,
                         builder.getStringAttr("unwrap_or"), ValueRange{zero})
                     .getResult(0);
      } else {
        Value message =
            builder
                .create<emitrust::LiteralOp>(
                    loc,
                    emitrust::OpaqueType::get(builder.getContext(), "&str"),
                    builder.getStringAttr("\"null pointer read\""))
                .getResult();
        cursor = builder
                     .create<emitrust::MethodCallOp>(
                         loc, TypeRange{i64Type}, staged.tmpPlace,
                         builder.getStringAttr("expect"), ValueRange{message})
                     .getResult(0);
      }
      builder.create<memref::StoreOp>(loc, cursor, staged.cursorCell);
    }
  }
  if (callOp->getNumResults() == 0)
    return Value();
  return callOp->getResult(0);
}

/// FR-86: conservative purity for a slice-argument OFFSET index. The
/// interception evaluates the index exactly ONCE, ahead of the call, so
/// admission requires only that the evaluation itself has no side
/// effects (nothing to duplicate, nothing to reorder). Admitted:
/// integer/character literals, enum constants, reads of local variables
/// and parameters, member chains over those (an `->` root reads its
/// pointer — also pure), sizeof/alignof, casts and parens, unary
/// +/-/~/! and non-assignment binary arithmetic over pure operands.
/// Everything else — a CALL, an inc/dec, any assignment, and every
/// unlisted node — is treated as impure, which DECLINES the
/// interception so the historical located decay rejection fires
/// unchanged (the pinned frontier).
static bool isPureSliceCursorExpr(const clang::Expr *expr) {
  expr = stripTrivia(expr);
  if (llvm::isa<clang::IntegerLiteral, clang::CharacterLiteral>(expr))
    return true;
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
    if (llvm::isa<clang::EnumConstantDecl>(ref->getDecl()))
      return true;
    const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
    return var && var->hasLocalStorage();
  }
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr))
    return isPureSliceCursorExpr(member->getBase());
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(expr))
    return isPureSliceCursorExpr(cast->getSubExpr());
  if (llvm::isa<clang::UnaryExprOrTypeTraitExpr>(expr))
    return true;
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
    switch (unary->getOpcode()) {
    case clang::UO_Plus:
    case clang::UO_Minus:
    case clang::UO_Not:
    case clang::UO_LNot:
      return isPureSliceCursorExpr(unary->getSubExpr());
    default:
      return false;
    }
  }
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expr)) {
    switch (binary->getOpcode()) {
    case clang::BO_Add:
    case clang::BO_Sub:
    case clang::BO_Mul:
    case clang::BO_Div:
    case clang::BO_Rem:
    case clang::BO_Shl:
    case clang::BO_Shr:
    case clang::BO_And:
    case clang::BO_Or:
    case clang::BO_Xor:
      return isPureSliceCursorExpr(binary->getLHS()) &&
             isPureSliceCursorExpr(binary->getRHS());
    default:
      return false;
    }
  }
  return false;
}

bool CImporter::matchMemberArraySliceArg(
    const clang::MemberExpr *member, bool isMutParam,
    const clang::VarDecl *&chainRoot,
    SmallVectorImpl<const clang::FieldDecl *> &path) {
  // The decayed leaf must be a fixed-extent array FIELD — the whole
  // region the slice covers. (A flexible/zero-length tail has no extent;
  // an incomplete array is not a ConstantArrayType and never matches —
  // EXCEPT, FR-94, an ADMITTED u8 FAM tail, whose owned `Vec<u8>` member
  // IS the whole region and slices like any member array. FR-95 keeps
  // this byte-window slice path u8-only: a TYPED tail's slice argument
  // declines here and stays on its located rejection.)
  const auto *leaf = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
  if (!leaf)
    return false;
  bool famU8Tail =
      famTailField(leaf->getParent()) == leaf &&
      isU8ScalarType(
          astContext().getAsArrayType(leaf->getType())->getElementType());
  if (!leaf->getType().getCanonicalType()->isConstantArrayType() &&
      !famU8Tail)
    return false;
  return matchMemberChainRoot(member, isMutParam, chainRoot, path);
}

bool CImporter::matchMemberChainRoot(
    const clang::MemberExpr *member, bool isMutParam,
    const clang::VarDecl *&chainRoot,
    SmallVectorImpl<const clang::FieldDecl *> &path) {
  SmallVector<const clang::FieldDecl *, 4> reversed;
  const clang::MemberExpr *link = member;
  while (true) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(link->getMemberDecl());
    // Union arms OVERLAP by construction (disjoint-field admission is a
    // struct-only fact), and a byte-region record's members are windows
    // of one region base, not places; both keep the verbatim rejection.
    if (!field || field->getParent()->isUnion() ||
        isByteRegionRecord(field->getParent()))
      return false;
    reversed.push_back(field);
    const clang::Expr *base = stripTrivia(link->getBase());
    if (link->isArrow()) {
      // Arrow only at the chain ROOT, through a ref/mut_ref-struct
      // pointer parameter (its member place is the pointee itself,
      // deref + member) or — FR-86 mechanism A — through a DECOMPOSED
      // struct-pointer parameter. A null-compared struct pointer
      // (`if (s == (Cp)0) return;` — tinycrypt null-checks every
      // context pointer) demotes to the (slice-of-struct base, i64
      // cursor) representation, and its member place is exactly what
      // `s->n` already lowers to: subscript the deref'd slice base at
      // the CURRENT cursor, then project the member. Admission is
      // deliberately narrow — the root must be the parameter's OWN
      // self-based region (pointerLocals base == the var itself; no
      // nullable discriminant, no multi-base, no literal/heap backing,
      // no member base), its symbols-held place must be either the
      // deref'd slice-of-struct base (the Phase-1b slice
      // decomposition) or the owner method's array-of-struct data
      // place (the owner-region pointer-parameter form of the same
      // decomposition — receiver methods always hold `&mut self`, so
      // mutability is given there), and a MUTABLE slice cannot borrow
      // through a SHARED (const-pointee) slice region. Every other
      // decomposed shape (and an erased-global return base, which
      // resolves to a staged copy) falls through to the historical
      // rejection.
      if (isDecomposedPointerExpr(base)) {
        const auto *ref =
            llvm::dyn_cast<clang::DeclRefExpr>(base->IgnoreParenImpCasts());
        const auto *var =
            ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
        if (!var || !var->hasLocalStorage())
          return false;
        // FR-94: an owned FAM-record local roots the chain at its own
        // struct place (the degenerate whole-object decomposition); its
        // symbols place is the owned lvalue, always mutably borrowable.
        if (famAllocLocals.contains(var)) {
          auto ownedIt = symbols.find(var);
          if (ownedIt == symbols.end())
            return false;
          auto ownedLValue =
              llvm::dyn_cast<emitrust::LValueType>(ownedIt->second.getType());
          if (!ownedLValue ||
              !llvm::isa<emitrust::StructType>(ownedLValue.getValueType()))
            return false;
          chainRoot = var;
          break;
        }
        auto localIt = pointerLocals.find(var);
        if (localIt == pointerLocals.end())
          return false;
        const PointerLocalInfo &info = localIt->second;
        if (info.base != var || info.nonNullCell || info.baseIndexCell ||
            info.literalBacking || info.backing || info.member)
          return false;
        auto it = symbols.find(var);
        if (it == symbols.end())
          return false;
        auto lvalue =
            llvm::dyn_cast<emitrust::LValueType>(it->second.getType());
        if (!lvalue)
          return false;
        Type regionElement;
        bool ownerDataRegion = false;
        if (auto sliceType =
                llvm::dyn_cast<emitrust::SliceType>(lvalue.getValueType())) {
          regionElement = sliceType.getElementType();
        } else if (auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(
                       lvalue.getValueType())) {
          regionElement = arrayType.getElementType();
          ownerDataRegion = true;
        }
        if (!regionElement || !llvm::isa<emitrust::StructType>(regionElement))
          return false;
        if (isMutParam && !ownerDataRegion) {
          auto deref = it->second.getDefiningOp<emitrust::DerefOp>();
          if (!deref || !llvm::isa<emitrust::MutRefType>(
                            deref->getOperand(0).getType()))
            return false;
        }
        chainRoot = var;
        break;
      }
      // The pointer READ under `->` arrives as an LValueToRValue cast
      // over the parameter reference; peel it to name the root.
      const auto *ref =
          llvm::dyn_cast<clang::DeclRefExpr>(base->IgnoreParenImpCasts());
      const auto *var =
          ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
      if (!var || !var->hasLocalStorage())
        return false;
      auto it = symbols.find(var);
      if (it == symbols.end())
        return false;
      Type held = it->second.getType();
      if (auto lvalue = llvm::dyn_cast<emitrust::LValueType>(held))
        held = lvalue.getValueType();
      Type pointeeType;
      if (auto mutRef = llvm::dyn_cast<emitrust::MutRefType>(held)) {
        pointeeType = mutRef.getPointee();
      } else if (auto sharedRef = llvm::dyn_cast<emitrust::RefType>(held)) {
        // `&S` cannot yield `&mut s.field`; only a shared slice may
        // borrow through a shared struct reference.
        if (isMutParam)
          return false;
        pointeeType = sharedRef.getPointee();
      } else {
        return false;
      }
      if (!llvm::isa<emitrust::StructType>(pointeeType))
        return false;
      chainRoot = var;
      break;
    }
    if (const auto *inner = llvm::dyn_cast<clang::MemberExpr>(base)) {
      link = inner;
      continue;
    }
    // Dot chain root: an addressable LOCAL struct place. A global root
    // is refused here — its member place would be a staged local copy,
    // and a mutable slice of the copy would silently lose the callee's
    // writes.
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(base);
    const auto *var =
        ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
    if (!var || !var->hasLocalStorage())
      return false;
    auto it = symbols.find(var);
    if (it == symbols.end())
      return false;
    auto lvalue = llvm::dyn_cast<emitrust::LValueType>(it->second.getType());
    if (!lvalue || !llvm::isa<emitrust::StructType>(lvalue.getValueType()))
      return false;
    chainRoot = var;
    break;
  }
  path.assign(reversed.rbegin(), reversed.rend());
  return true;
}

std::optional<std::pair<const clang::VarDecl *, const clang::FieldDecl *>>
CImporter::classifyMemberArrayDecay(const clang::MemberExpr *member) {
  // C path only (the FR-93 gate): the C++ importer keeps the historical
  // non-address rejection for every member decay.
  if (astContext().getLangOpts().CPlusPlus)
    return std::nullopt;
  const auto *leaf = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
  if (!leaf || leaf->getParent()->isUnion())
    return std::nullopt;
  if (isByteRegionRecord(leaf->getParent())) {
    // FR-91 window root. The leaf gate mirrors
    // `tryEmitByteRegionMemberWindow`: a fixed-extent nonzero u8 array
    // only — a flexible/zero-length tail has no extent (the heatshrink
    // FAM pins), and a nested record never decays.
    const clang::ConstantArrayType *leafArray =
        astContext().getAsConstantArrayType(leaf->getType());
    if (!leafArray || leafArray->getSize().isZero() ||
        !isU8ScalarType(leafArray->getElementType()))
      return std::nullopt;
    const clang::MemberExpr *link = member;
    while (true) {
      const auto *field =
          llvm::dyn_cast<clang::FieldDecl>(link->getMemberDecl());
      if (!field || field->getParent()->isUnion() ||
          !isByteRegionRecord(field->getParent()))
        return std::nullopt;
      const clang::Expr *base = stripTrivia(link->getBase());
      if (link->isArrow()) {
        // Arrow only at the root, through a byte-region struct-pointer
        // PARAMETER — its region place is the prologue's deref'd byte
        // slice, the exact base the absolute cursor walks.
        const auto *ref =
            llvm::dyn_cast<clang::DeclRefExpr>(base->IgnoreParenImpCasts());
        const auto *param =
            ref ? llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()) : nullptr;
        if (!param || !isPointerType(param->getType()))
          return std::nullopt;
        return std::make_pair(static_cast<const clang::VarDecl *>(param),
                              static_cast<const clang::FieldDecl *>(nullptr));
      }
      if (const auto *inner = llvm::dyn_cast<clang::MemberExpr>(base)) {
        link = inner;
        continue;
      }
      // Dot root: a directly named LOCAL byte-region aggregate — its own
      // flat byte array. A global root's place would be a staged copy
      // (writes silently lost), so it keeps the historical rejection.
      const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(base);
      const auto *var =
          ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
      if (!var || !var->hasLocalStorage() ||
          !isByteRegionAggregate(var->getType()))
        return std::nullopt;
      return std::make_pair(var,
                            static_cast<const clang::FieldDecl *>(nullptr));
    }
  }
  // Typed root: a nonzero fixed-extent array leaf over a SINGLE-link
  // chain — nested chains, 2D member rows (the decayed operand is a
  // subscript, never a MemberExpr), and global roots keep the
  // historical rejection. FR-94: an ADMITTED FAM tail is the one
  // extent-less leaf that classifies — its owned `Vec<u8>` member is the
  // backing region.
  bool famLeaf = famTailField(leaf->getParent()) == leaf;
  const clang::ConstantArrayType *leafArray =
      astContext().getAsConstantArrayType(leaf->getType());
  if ((!leafArray || leafArray->getSize().isZero()) && !famLeaf)
    return std::nullopt;
  const clang::Expr *base = stripTrivia(member->getBase());
  if (member->isArrow()) {
    const auto *ref =
        llvm::dyn_cast<clang::DeclRefExpr>(base->IgnoreParenImpCasts());
    // FR-94: an owned FAM-record LOCAL is an admitted arrow root for its
    // own tail (`p = d->buffers` / `p = &d->buffers[k]`): the member place
    // projects on the owned struct local.
    if (famLeaf && ref)
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
          var && !llvm::isa<clang::ParmVarDecl>(var) &&
          famAllocLocals.contains(var))
        return std::make_pair(var, leaf);
    // FR-98: a recognized MEMBER-READ local over a lifted member-held FAM
    // field (FR-96, `hsi = hse->search_index`) is an admitted arrow root
    // for the POINTEE record's own tail (`index = hsi->index`,
    // heatshrink_encoder.c:425): the local names no place of its own, so
    // each use re-projects the member's Option payload fresh and
    // subscripts the projected tail Vec (emitPointerPlace's
    // famMemberLocals arm). The famOptionMemberPointee gate is the poison
    // mirror of famOptionMemberOf: an unlifted or poisoned field keeps the
    // historical non-address rejection.
    if (famLeaf && ref)
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
          var && !llvm::isa<clang::ParmVarDecl>(var))
        if (const clang::MemberExpr *rootInit = famMemberLocals.lookup(var))
          if (const auto *rootField = llvm::dyn_cast<clang::FieldDecl>(
                  rootInit->getMemberDecl()))
            if (const clang::RecordDecl *pointee =
                    famOptionMemberPointee(rootField);
                pointee && pointee->getDefinition() ==
                               leaf->getParent()->getDefinition())
              return std::make_pair(var, leaf);
    const auto *param =
        ref ? llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()) : nullptr;
    if (!param)
      return std::nullopt;
    clang::QualType baseType = param->getType().getCanonicalType();
    if (!baseType->isPointerType())
      return std::nullopt;
    const clang::RecordDecl *record =
        baseType->getPointeeType()->getAsRecordDecl();
    if (!record ||
        record->getCanonicalDecl() != leaf->getParent()->getCanonicalDecl())
      return std::nullopt;
    return std::make_pair(static_cast<const clang::VarDecl *>(param), leaf);
  }
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(base);
  const auto *var =
      ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
  if (!var || !var->hasLocalStorage() ||
      !var->getType().getCanonicalType()->getAsRecordDecl())
    return std::nullopt;
  return std::make_pair(var, leaf);
}

FailureOr<PtrExprValue>
CImporter::emitMemberArrayDecayValue(Location loc,
                                     const clang::MemberExpr *member,
                                     const clang::VarDecl *root,
                                     const clang::FieldDecl *field) {
  if (!field) {
    // Byte-region window: the absolute byte cursor is the field's layout
    // offset plus the root's own runtime cursor (FR-91 convention).
    FailureOr<ByteRegionRef> region = resolveByteRegionRef(member, nullptr);
    if (failed(region))
      return failure();
    // Defensive net: the classifier admits local roots only, so the
    // resolved place must be the root's own symbols place — a staged
    // copy here would silently decouple the cursor from the object.
    auto it = symbols.find(root);
    if (it == symbols.end() || region->place != it->second)
      return emitError(loc)
             << "unsupported: pointer assigned a non-address value";
    return PtrExprValue{root, byteRegionOffset(loc, *region)};
  }
  PtrExprValue value{
      root, createIntConstant(loc, builder.getIntegerType(64), 0)};
  value.member = field;
  return value;
}

FailureOr<bool> CImporter::tryEmitByteRegionMemberWindow(
    Location loc, const clang::MemberExpr *member,
    const clang::Expr *cursorIndex, bool isMutParam, Value &place,
    Value &cursor, Type &element, const clang::VarDecl *&chainRoot) {
  // The leaf must be a fixed-extent u8 array: a flexible/zero-length
  // tail has no extent (the heatshrink `hsd->buffers` FAM sites stay
  // located at the decay), and a non-array or nested-record leaf is not
  // a window this interception proves.
  const auto *leaf = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
  if (!leaf)
    return false;
  const clang::ConstantArrayType *leafArray =
      astContext().getAsConstantArrayType(leaf->getType());
  if (!leafArray || leafArray->getSize().isZero() ||
      !isU8ScalarType(leafArray->getElementType()))
    return false;
  // FR-86 purity gate, unchanged: the offset index is evaluated exactly
  // once, ahead of the borrow, so an impure index declines.
  std::optional<llvm::APSInt> constantCursor;
  if (cursorIndex) {
    constantCursor = cursorIndex->getIntegerConstantExpr(astContext());
    if (!constantCursor && !isPureSliceCursorExpr(cursorIndex))
      return false;
  }
  // Chain walk — AST-only, so a decline emits no IR. Every link must be
  // a non-union byte-region field (a union arm keeps the historical
  // rejection), and the root must be LOCAL: a global root's region
  // place would be a staged copy, and a mutable window of the copy
  // would silently lose the callee's writes (the typed member path
  // refuses globals identically). An arrow root through a CONST
  // (shared) region cannot yield a `&mut` window.
  const clang::MemberExpr *link = member;
  const clang::VarDecl *rootVar = nullptr;
  while (true) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(link->getMemberDecl());
    if (!field || field->getParent()->isUnion() ||
        !isByteRegionRecord(field->getParent()))
      return false;
    const clang::Expr *base = stripTrivia(link->getBase());
    if (link->isArrow()) {
      clang::QualType baseType = base->getType().getCanonicalType();
      if (!baseType->isPointerType())
        return false;
      if (isMutParam && baseType->getPointeeType().isConstQualified())
        return false;
      const auto *ref =
          llvm::dyn_cast<clang::DeclRefExpr>(base->IgnoreParenImpCasts());
      const auto *var =
          ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
      if (!var || !var->hasLocalStorage())
        return false;
      rootVar = var;
      break;
    }
    if (const auto *inner = llvm::dyn_cast<clang::MemberExpr>(base)) {
      link = inner;
      continue;
    }
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(base);
    const auto *var =
        ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
    if (!var || !var->hasLocalStorage())
      return false;
    rootVar = var;
    break;
  }
  // Resolution may still reject, LOCATED: a null-compared or otherwise
  // demoted pointer root hits `resolveByteRegionPointer`'s existing
  // literalBacking/baseIndex/nonNull/member rejections — that frontier
  // stays where the byte-region contract put it.
  FailureOr<ByteRegionRef> region = resolveByteRegionRef(member, nullptr);
  if (failed(region))
    return failure();
  Type ui8Type =
      IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  auto lvalueType =
      llvm::dyn_cast<emitrust::LValueType>(region->place.getType());
  Type baseElement;
  if (lvalueType) {
    if (auto arrayType =
            llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType()))
      baseElement = arrayType.getElementType();
    else if (auto sliceType =
                 llvm::dyn_cast<emitrust::SliceType>(lvalueType.getValueType()))
      baseElement = sliceType.getElementType();
  }
  if (baseElement != ui8Type)
    return emitError(loc) << "unsupported pointer target place";
  // The window cursor: the region's byte offset (constant layout offset
  // plus the pointer parameter's runtime cursor) plus the FR-86 offset
  // index. A constant index folds into the constant part.
  ByteRegionRef window = *region;
  if (constantCursor)
    window.constOff += constantCursor->getExtValue();
  Value cursorValue = byteRegionOffset(loc, window);
  if (cursorIndex && !constantCursor) {
    FailureOr<Value> rawIndex = emitRValue(cursorIndex);
    if (failed(rawIndex))
      return failure();
    if (!llvm::isa<IntegerType>((*rawIndex).getType()))
      return emitError(loc) << "unsupported subscript index type";
    Value index64 = castToIntType(loc, *rawIndex, builder.getIntegerType(64));
    cursorValue =
        builder.create<arith::AddIOp>(loc, cursorValue, index64).getResult();
  }
  place = region->place;
  cursor = cursorValue;
  element = ui8Type;
  chainRoot = rootVar;
  return true;
}

FailureOr<bool> CImporter::tryEmitMemberArraySlicePlace(
    Location loc, const clang::Expr *stripped, bool isMutParam,
    Value &place, Value &cursor, Type &element,
    const clang::VarDecl *&chainRoot,
    SmallVectorImpl<const clang::FieldDecl *> &path) {
  const clang::Expr *decayedOperand = nullptr;
  const clang::Expr *cursorIndex = nullptr; // null => whole member.
  if (const auto *memberDecay =
          llvm::dyn_cast<clang::ImplicitCastExpr>(stripped);
      memberDecay &&
      memberDecay->getCastKind() == clang::CK_ArrayToPointerDecay) {
    decayedOperand = stripTrivia(memberDecay->getSubExpr());
  } else if (const auto *addrOf =
                 llvm::dyn_cast<clang::UnaryOperator>(stripped);
             addrOf && addrOf->getOpcode() == clang::UO_AddrOf) {
    // `&s.m[k]`: address of a subscript over the decayed member.
    if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(
            stripTrivia(addrOf->getSubExpr())))
      if (const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(
              stripTrivia(subscript->getBase()));
          decay && decay->getCastKind() == clang::CK_ArrayToPointerDecay) {
        decayedOperand = stripTrivia(decay->getSubExpr());
        cursorIndex = subscript->getIdx();
      }
  } else if (const auto *add =
                 llvm::dyn_cast<clang::BinaryOperator>(stripped);
             add && add->getOpcode() == clang::BO_Add) {
    // `s.m + k` (C also admits the commuted `k + s.m`).
    const clang::Expr *pointerSide = stripTrivia(add->getLHS());
    const clang::Expr *indexSide = stripTrivia(add->getRHS());
    if (!pointerSide->getType()->isPointerType())
      std::swap(pointerSide, indexSide);
    if (const auto *decay =
            llvm::dyn_cast<clang::ImplicitCastExpr>(pointerSide);
        decay && decay->getCastKind() == clang::CK_ArrayToPointerDecay) {
      decayedOperand = stripTrivia(decay->getSubExpr());
      cursorIndex = indexSide;
    }
  }
  const auto *member =
      llvm::dyn_cast_or_null<clang::MemberExpr>(decayedOperand);
  // FR-91: a member chain ending in a BYTE-REGION record routes through
  // the region-window arm instead of the typed member matcher (which
  // deliberately declines byte-region parents). The window reports an
  // EMPTY field path: region windows share ONE slice place, so aliasing
  // must key on the root alone — a second window of the same root in
  // one call collides, and the byte-family same-root pair rides
  // `copy_within` on the whole region with absolute byte cursors.
  if (member)
    if (const auto *leafField =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
        leafField && isByteRegionRecord(leafField->getParent())) {
      FailureOr<bool> window = tryEmitByteRegionMemberWindow(
          loc, member, cursorIndex, isMutParam, place, cursor, element,
          chainRoot);
      if (failed(window) || !*window)
        return window;
      path.clear();
      return true;
    }
  std::optional<llvm::APSInt> constantCursor;
  if (member && cursorIndex)
    constantCursor = cursorIndex->getIntegerConstantExpr(astContext());
  const clang::VarDecl *root = nullptr;
  SmallVector<const clang::FieldDecl *, 2> matchedPath;
  if (!member ||
      (cursorIndex && !constantCursor &&
       !isPureSliceCursorExpr(cursorIndex)) ||
      !matchMemberArraySliceArg(member, isMutParam, root, matchedPath))
    return false;
  FailureOr<Value> memberPlace =
      emitMemberLValue(member, loc, /*writeback=*/nullptr);
  if (failed(memberPlace))
    return failure();
  auto memberLValue =
      llvm::dyn_cast<emitrust::LValueType>((*memberPlace).getType());
  Type memberElement;
  if (memberLValue) {
    if (auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(
            memberLValue.getValueType()))
      memberElement = arrayType.getElementType();
    // FR-94: an admitted FAM tail's member place is the owned `Vec<u8>`
    // field; its element is the byte the slice window carries.
    else if (auto opaque = llvm::dyn_cast<emitrust::OpaqueType>(
                 memberLValue.getValueType());
             opaque && opaque.getValue() == "Vec<u8>")
      memberElement =
          IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  }
  if (!memberElement)
    return emitError(loc) << "unsupported pointer target place";
  Value cursorValue;
  if (constantCursor) {
    cursorValue = createIntConstant(loc, builder.getIntegerType(64),
                                    constantCursor->getExtValue());
  } else if (cursorIndex) {
    FailureOr<Value> rawIndex = emitRValue(cursorIndex);
    if (failed(rawIndex))
      return failure();
    if (!llvm::isa<IntegerType>((*rawIndex).getType()))
      return emitError(loc) << "unsupported subscript index type";
    cursorValue = castToIntType(loc, *rawIndex, builder.getIntegerType(64));
  } else {
    cursorValue = createIntConstant(loc, builder.getIntegerType(64), 0);
  }
  place = *memberPlace;
  cursor = cursorValue;
  element = memberElement;
  chainRoot = root;
  path.assign(matchedPath.begin(), matchedPath.end());
  return true;
}

/// FR-89: returns the operand of a void*-MEDIATED pointer cast (the
/// implicit bitcast Sema inserts converting to or from `void *` at a
/// call argument, or its explicit spelling) or null for every other
/// cast. This is `peelPointerCast`'s pointee-wildcard rule (CTS-P9)
/// narrowed to the void-on-one-side case for the borrow-ARGUMENT path:
/// same-pointee and qualification-only casts are NOT peeled here — the
/// slice branch's own CK_NoOp strip and the string-literal head already
/// own those spellings, and a general peel would newly claim typed-path
/// cast spellings this FR does not prove. A volatile-qualified pointee
/// blocks the peel so the site keeps a located rejection (C99-7).
static const clang::Expr *
peelVoidMediatedArgumentCast(clang::ASTContext &context,
                             const clang::Expr *expr) {
  const auto *cast = llvm::dyn_cast<clang::CastExpr>(expr);
  if (!cast || (!llvm::isa<clang::CStyleCastExpr>(cast) &&
                !llvm::isa<clang::ImplicitCastExpr>(cast)))
    return nullptr;
  if (cast->getCastKind() != clang::CK_NoOp &&
      cast->getCastKind() != clang::CK_BitCast)
    return nullptr;
  clang::QualType from = cast->getSubExpr()->getType().getCanonicalType();
  clang::QualType to = cast->getType().getCanonicalType();
  if (!isDataPointer(from) || !isDataPointer(to))
    return nullptr;
  clang::QualType fromPointee = from->getPointeeType().getCanonicalType();
  clang::QualType toPointee = to->getPointeeType().getCanonicalType();
  if (fromPointee.isVolatileQualified() || toPointee.isVolatileQualified())
    return nullptr;
  if (!fromPointee->isVoidType() && !toPointee->isVoidType())
    return nullptr;
  return cast->getSubExpr();
}

FailureOr<Value> CImporter::emitBorrowArgument(
    Location loc, const clang::Expr *argument, Type paramType,
    const clang::VarDecl *&root,
    SmallVectorImpl<const clang::FieldDecl *> *rootPath) {
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
    // FR-89: a member-array argument to a void*-ADMITTED byte-cursor
    // parameter (FR-71 body-scan / FR-75 consensus) arrives WRAPPED in
    // the implicit void* BitCast Sema inserts at the conversion site
    // (`bitcast<void*>(decay(prng->key))` — tinycrypt's
    // `_set(prng->key, 0x00, sizeof(prng->key))` at hmac_prng.c:143 and
    // `tc_hmac_update(&prng->h, prng->v, ...)` at :88). The FR-74/86
    // matcher below sees only decay / `&s.m[k]` / `s.m + k` heads, so
    // the void*-mediated cast is peeled here — AFTER the string-literal
    // head (a literal to an i8-element void* param keeps its exact
    // pre-FR-89 pointer-rvalue lowering, and a ui8-element one its
    // element-mismatch rejection: the peel provably admits nothing on
    // the literal shapes) and BEFORE the member matcher, on the
    // borrow-ARGUMENT path only. Every shape the matcher then declines
    // falls through to `emitPointerRValue` on the UNPEELED argument,
    // so all non-member void* arguments keep their exact current
    // behavior; element agreement against the admitted byte element and
    // the (root, field-path) aliasing guard compose downstream
    // unchanged.
    while (const clang::Expr *peeled =
               peelVoidMediatedArgumentCast(astContext(), strippedArg)) {
      strippedArg = stripTrivia(peeled);
      while (const auto *noop =
                 llvm::dyn_cast<clang::ImplicitCastExpr>(strippedArg)) {
        if (noop->getCastKind() != clang::CK_NoOp)
          break;
        strippedArg = stripTrivia(noop->getSubExpr());
      }
    }
    // FR-74: a MEMBER-ARRAY argument (`bump(s.iv, s.n)` — tinycrypt's
    // compress shape): a dot/arrow projection chain ending at an
    // array-typed field decays to the same whole-region slice a top-level
    // array does — `slice_of` over the member PLACE at cursor 0. The
    // interception lives HERE, in the slice-argument position (and,
    // FR-87, in the hosted byte-family REGION position through the
    // shared `tryEmitMemberArraySlicePlace` core), and
    // NOT in `emitPointerRValue`'s decay case: member decay in
    // non-argument positions (a pointer local bound to `s.iv`) must keep
    // its verbatim rejection, because the pointer decomposition has no
    // representation for a member-rooted region. Unadmitted chains fall
    // through to `emitPointerRValue`'s historical rejection unchanged.
    //
    // FR-86 extends the SAME interception with a NONZERO cursor for the
    // OFFSET spellings `s.m + k` and `&s.m[k]` (tinycrypt's
    // `add_round_key(state, s->words + Nb*Nr)`): `slice_of` already
    // takes a cursor operand, so the offset argument is the member
    // place at cursor k. The index is evaluated exactly ONCE, ahead of
    // the call: an integer-constant index folds to a literal i64
    // cursor, and a runtime index is admitted only when conservatively
    // PURE (`isPureSliceCursorExpr`) — an impure index (a call, an
    // inc/dec) DECLINES the interception so the historical located
    // decay rejection fires unchanged.
    {
      Value memberPlace;
      Value memberCursor;
      Type memberElement;
      const clang::VarDecl *chainRoot = nullptr;
      SmallVector<const clang::FieldDecl *, 2> chainPath;
      FailureOr<bool> matched = tryEmitMemberArraySlicePlace(
          loc, strippedArg, isMutParam, memberPlace, memberCursor,
          memberElement, chainRoot, chainPath);
      if (failed(matched))
        return failure();
      if (*matched) {
        if (memberElement != sliceType.getElementType())
          return emitError(loc)
                 << "unsupported: argument element type does not "
                    "match the slice parameter";
        root = chainRoot;
        if (rootPath)
          rootPath->assign(chainPath.begin(), chainPath.end());
        return builder
            .create<emitrust::SliceOfOp>(loc, paramType, memberPlace,
                                         memberCursor,
                                         /*is_mut=*/isMutParam)
            .getResult();
      }
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
    // FR-93: a member-array-backed pointer (`p = s->arr; f(p);`) borrows
    // ONLY its field — slice_of over the freshly projected member place
    // at the current cursor, keyed (root, field) so `emitCall`'s
    // prefix-overlap aliasing guard sees exactly the FR-74 member key.
    // Degenerate (scalar-member, cursorless) pointers keep the paths
    // below byte-for-byte.
    if (pointer->member && pointer->cursor) {
      auto memberIt = symbols.find(pointer->base);
      if (memberIt == symbols.end())
        return emitError(loc) << "unsupported: pointer target '"
                              << pointer->base->getName()
                              << "' is not an importable place";
      FailureOr<Value> memberPlace =
          projectPointerMemberBase(loc, memberIt->second, pointer->member);
      if (failed(memberPlace))
        return failure();
      auto memberLValue =
          llvm::dyn_cast<emitrust::LValueType>((*memberPlace).getType());
      auto memberArray =
          memberLValue ? llvm::dyn_cast<emitrust::ArrayType>(
                             memberLValue.getValueType())
                       : emitrust::ArrayType();
      if (!memberArray)
        return emitError(loc) << "unsupported pointer target place";
      if (memberArray.getElementType() != sliceType.getElementType())
        return emitError(loc) << "unsupported: argument element type does "
                                 "not match the slice parameter";
      if (rootPath)
        rootPath->assign(1, pointer->member);
      return builder
          .create<emitrust::SliceOfOp>(loc, paramType, *memberPlace,
                                       pointer->cursor, /*is_mut=*/isMutParam)
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
  // FR-90: a member-ADDRESS argument (`&s->field`, field STRUCT-typed —
  // tinycrypt's `tc_aes_encrypt(..., &ctx->key)` and hmac's
  // `tc_sha256_init(&ctx->hash_state)`) whose chain roots at a
  // DECOMPOSED (null-compared) struct-pointer parameter. The null
  // compare demotes the root from the ref/mut_ref receiver convention —
  // whose `&s->field` already borrows through the historical
  // emitLValue+addr_of path below — to the (slice-of-struct base, i64
  // cursor) decomposition, and the decomposed routing has no
  // member-address representation (`classifyMemberAddress` leaves arrow
  // roots unhandled), so the shape historically rejected "unsupported
  // pointer target expression". The member PLACE, however, is exactly
  // what FR-86 mechanism A already builds for member-ARRAY arguments of
  // the same roots — subscript the deref'd base at the CURRENT cursor,
  // then project the field chain — so the address is that place's
  // borrow at the PARAMETER's own mutability. Admission mirrors the
  // FR-74/86 matcher root-for-root (`matchMemberChainRoot`: the
  // decomposed root must be the parameter's OWN self-based region,
  // union arms and byte-region records decline, a mutable borrow cannot
  // go through a shared slice region); the leaf must be a plain
  // STRUCT-typed field — a scalar, union, or array leaf declines so the
  // historical located rejection fires unchanged. (root, field-path)
  // feed emitCall's prefix-overlap aliasing guard, so disjoint sibling
  // borrows of one root — including the mixed addr_of + slice_of pair
  // `f(&prng->h, prng->key, ...)` — are admitted while any overlap
  // (the same field twice, a whole-root forward plus a member) stays a
  // located rejection. Gated on the DECOMPOSED root: a non-decomposed
  // argument keeps the historical paths below byte-for-byte. The
  // qualification NoOp Sema wraps a `struct T *` argument in at a
  // `const struct T *` parameter (the FR-80 shared-const-struct
  // requirement convention, and const-pointee params generally) is
  // peeled INSIDE this interception only — `isAddressOf` and the
  // routing below see the unpeeled argument, so every shape the
  // interception declines keeps its exact current behavior.
  const clang::Expr *addrExpr = stripped;
  while (const auto *noop = llvm::dyn_cast<clang::ImplicitCastExpr>(addrExpr)) {
    if (noop->getCastKind() != clang::CK_NoOp)
      break;
    addrExpr = stripTrivia(noop->getSubExpr());
  }
  const auto *memberAddrOf = llvm::dyn_cast<clang::UnaryOperator>(addrExpr);
  if (memberAddrOf && memberAddrOf->getOpcode() == clang::UO_AddrOf &&
      involvesDecomposedPointer(argument) &&
      llvm::isa<emitrust::StructType>(pointee)) {
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(
            stripTrivia(memberAddrOf->getSubExpr()))) {
      const auto *leaf =
          llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
      const clang::VarDecl *chainRoot = nullptr;
      SmallVector<const clang::FieldDecl *, 2> chainPath;
      if (leaf && leaf->getType().getCanonicalType()->isStructureType() &&
          matchMemberChainRoot(member, isMutParam, chainRoot, chainPath)) {
        FailureOr<Value> memberPlace =
            emitMemberLValue(member, loc, /*writeback=*/nullptr);
        if (failed(memberPlace))
          return failure();
        auto memberLValue =
            llvm::dyn_cast<emitrust::LValueType>((*memberPlace).getType());
        if (!memberLValue || memberLValue.getValueType() != pointee)
          return emitError(loc) << "unsupported: argument type does not "
                                   "match the pointer parameter";
        root = chainRoot;
        if (rootPath)
          rootPath->assign(chainPath.begin(), chainPath.end());
        return builder
            .create<emitrust::AddrOfOp>(loc, paramType, *memberPlace,
                                        isMutParam)
            .getResult();
      }
    }
  }
  // FR-92: pointer-to-array parameters (`state_t *` =
  // `uint8_t (*)[4][4]`, tiny-AES-c's Cipher family) map to whole-array
  // references (`&mut [[u8; 4]; 4]`), and two argument shapes are
  // admitted over that mapping:
  //   (1) FORWARDING — `Cipher(state, rk)` passes the caller's own
  //       pointer-to-array parameter through unchanged (aes.c:413/439:
  //       Cipher/InvCipher never index, they only forward). The block
  //       argument IS the borrow and Rust's implicit reborrow makes the
  //       bare value a legal call operand; `root` is the parameter so
  //       `emitCall`'s same-base aliasing guard covers `f(state, state)`.
  //   (2) THE 2D CAST — `Cipher((state_t*)buf, rk)` reinterprets a byte
  //       run as the 4x4 state at the SAME u8 element (layout identity,
  //       never a transmute): the cast's source reslices to `&mut [u8]`
  //       through the EXISTING slice-argument machinery (a recursive
  //       borrow at a synthetic `&mut [u8]` parameter type covers the
  //       three aes.c flavors — a plain `uint8_t*` parameter at its
  //       cursor, a WALKING cursor at its current offset, a local byte
  //       array at cursor 0), and the on-demand module-level
  //       `__emitrust_chunk_<R>x<C>` helper reshapes the byte view into
  //       `&mut [[u8; C]; R]`. A view too short for R*C panics at
  //       runtime (the argv_arg out-of-range precedent: panic where the
  //       C access was UB, the accepted refinement direction), but a
  //       STATICALLY undersized source — a constant-extent array base
  //       behind a constant cursor — rejects at import, because the
  //       importer can prove the panic on every execution. Only the
  //       mutable, u8-leaf, 2D destination is admitted; every declined
  //       shape falls through to the historical located rejections (an
  //       element-type-changing cast stays "unsupported pointer
  //       expression: CStyleCastExpr"; a `state_t*` LOCAL binding is not
  //       an argument and stays "pointer assigned a non-address value" —
  //       a whole-array reference has no (base, cursor) pointer
  //       decomposition, which is why the admission lives HERE and not in
  //       `emitPointerRValue`).
  // FR-100: the CALLEE-AWARE scalar analogue of FR-92's forwarding arm.
  // A caller whose own parameter is a scalar reference (the classifier's
  // TU-wide forwarding fixpoint kept it one precisely because this
  // callee's parameter is one too) passes the block argument through as
  // the bare operand, exactly as the whole-array forward below does; the
  // SYMBOL-TYPE EQUALITY is the proof the callee agreed on the class, so
  // a callee the fixpoint (or a later override, e.g. FR-76's
  // address-taken rule) classified as a Slice falls through to its
  // historical located rejection instead of borrowing one element of a
  // run the callee may walk. `root` is the parameter, so `emitCall`'s
  // same-base aliasing guard still covers `g(p, p)`. The arm is for
  // SCALAR references only: a slice-pointee parameter (or a CellSlice
  // one) carries a CURSOR, and the slice-argument machinery above
  // reslices it at that cursor — forwarding such a value bare would
  // silently hand the callee the whole region from offset zero, so both
  // region-shaped pointees are excluded here rather than relying on the
  // earlier branch to always return.
  if (!llvm::isa<emitrust::ArrayType>(pointee) &&
      !llvm::isa<emitrust::SliceType>(pointee)) {
    if (const clang::ParmVarDecl *forwarded = asPointerParamRef(stripped)) {
      auto it = symbols.find(forwarded);
      if (it != symbols.end() && it->second.getType() == paramType) {
        root = forwarded;
        return it->second;
      }
    }
  }
  if (auto destArray = llvm::dyn_cast<emitrust::ArrayType>(pointee)) {
    if (const clang::ParmVarDecl *forwarded = asPointerParamRef(stripped)) {
      auto it = symbols.find(forwarded);
      if (it != symbols.end() && it->second.getType() == paramType) {
        root = forwarded;
        return it->second;
      }
    }
    auto rowType =
        llvm::dyn_cast<emitrust::ArrayType>(destArray.getElementType());
    IntegerType leafType =
        rowType ? llvm::dyn_cast<IntegerType>(rowType.getElementType())
                : IntegerType();
    if (const auto *cast = llvm::dyn_cast<clang::CStyleCastExpr>(stripped);
        cast && isMutParam && rowType && leafType &&
        leafType.getWidth() == 8 && leafType.isUnsigned()) {
      Type byteSliceType =
          emitrust::MutRefType::get(emitrust::SliceType::get(leafType));
      FailureOr<Value> byteView = emitBorrowArgument(
          loc, cast->getSubExpr(), byteSliceType, root, rootPath);
      if (failed(byteView))
        return failure();
      int64_t rows = static_cast<int64_t>(destArray.getSize());
      int64_t cols = static_cast<int64_t>(rowType.getSize());
      // The import-time extent arm: a constant-extent array base behind
      // a constant start cursor either covers R*C bytes or provably
      // panics on every run — refuse the latter here, with a located
      // diagnostic, instead of emitting the guaranteed panic.
      if (auto sliceOf = (*byteView).getDefiningOp<emitrust::SliceOfOp>()) {
        auto baseLValue = llvm::dyn_cast<emitrust::LValueType>(
            sliceOf.getBase().getType());
        auto baseArray =
            baseLValue ? llvm::dyn_cast<emitrust::ArrayType>(
                             baseLValue.getValueType())
                       : emitrust::ArrayType();
        llvm::APInt start;
        if (baseArray &&
            matchPattern(sliceOf.getIndex(), m_ConstantInt(&start)) &&
            static_cast<int64_t>(baseArray.getSize()) -
                    start.getSExtValue() <
                rows * cols)
          return emitError(loc) << "unsupported: cast source region is "
                                   "smaller than the destination array";
      }
      neededChunkHelpers.insert({rows, cols});
      std::string helperName = ("__emitrust_chunk_" + llvm::Twine(rows) +
                                "x" + llvm::Twine(cols))
                                   .str();
      return builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{paramType}, builder.getStringAttr(helperName),
              /*args=*/ArrayAttr(), ValueRange{*byteView})
          .getResult(0);
    }
  }
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
    // wording when Pass A pinned one). FR-80 first: a DEGENERATE pointer
    // (whole-object or single-member address, no cursor, no null, no
    // multi-base) into a qualifying const-struct requirement resolves to
    // the requirement's own `emitrust.global_addr` — the dhcp/udp local
    // `ip_addr_t *p = &ip_addr_any; f(p);` flow — and only into a SHARED
    // parameter, the borrow shape the `&'static` getter lends.
    if (pointer->base && !pointer->base->hasLocalStorage()) {
      if (!isMutParam && !pointer->cursor && !pointer->baseIndex &&
          !pointer->literalBacking && !pointer->backing) {
        SmallVector<const clang::FieldDecl *, 2> fieldPath;
        if (pointer->member)
          fieldPath.push_back(pointer->member);
        if (addressableRequirementGlobal(pointer->base, fieldPath)) {
          FailureOr<Value> addr =
              emitRequirementGlobalAddress(loc, pointer->base, fieldPath);
          if (failed(addr))
            return failure();
          if ((*addr).getType() != paramType)
            return emitError(loc) << "unsupported: argument type does not "
                                     "match the pointer parameter";
          return *addr;
        }
      }
      return rejectGlobalPointerArgument(loc, pointer->base);
    }
    // FR-121: a string-literal element handed to a MUTABLE scalar-reference
    // parameter (`sink(__FILE__, ..)` against `fn sink(v0: &mut i8, ..)`,
    // the spdlog assert-fail shape) rematerializes a FRESH mutable backing
    // for this call, exactly as the mutable-slice branch above does for the
    // sliced form of the same argument. Borrowing the shared const backing
    // mutably instead emits `&mut v[i]` against a non-`mut` `let` — rustc
    // E0596, an emitted crate that cannot compile. The per-call copy is
    // sound for the same reason as the slice branch: writing through a
    // pointer to a string literal is undefined behavior, so no defined
    // program can observe that the callee got a copy; and the cached const
    // backing itself is untouched, so shared readers of the same literal
    // keep borrowing the original.
    if (pointer->literalBacking && isMutParam) {
      auto sourceVar =
          pointer->literalBacking.getDefiningOp<emitrust::VariableOp>();
      if (!sourceVar)
        return emitError(loc)
               << "unsupported: string-literal argument to a mutable "
                  "reference parameter has no backing to copy";
      pointer->literalBacking =
          builder
              .create<emitrust::VariableOp>(loc,
                                            pointer->literalBacking.getType(),
                                            sourceVar.getInitAttr(),
                                            /*isConst=*/false)
              .getResult();
    }
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
    // FR-80: `&g` (or an interior-member `&g.m` / `&((&g)->m)`) on a
    // qualifying extern const struct is an ADDRESS-CARRYING requirement:
    // the shared reference the parameter needs is exactly what the
    // requirement getter's `&'static T` supplies, so the address lowers to
    // `emitrust.global_addr` instead of rejecting. Every non-qualifying
    // shape keeps the historical located rejection below.
    {
      SmallVector<const clang::FieldDecl *, 2> fieldPath;
      const clang::VarDecl *reqGlobal =
          addressPathRoot(addrOf->getSubExpr(), fieldPath);
      if (reqGlobal && lookupGlobal(reqGlobal) &&
          addressableRequirementGlobal(reqGlobal, fieldPath)) {
        FailureOr<Value> addr =
            emitRequirementGlobalAddress(loc, reqGlobal, fieldPath);
        if (failed(addr))
          return failure();
        if ((*addr).getType() != paramType)
          return emitError(loc) << "unsupported: argument type does not "
                                   "match the pointer parameter";
        return *addr;
      }
    }
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

/// The (possibly cast-wrapped) plain read of a LOCAL pointer variable —
/// parameter or local — a call argument names; unlike `asLocalVarRef`,
/// parameters are included (a slice parameter is its own region base,
/// and the FR-93 split pairing keys on exactly that). Peels the
/// qualification / void* casts an argument position wraps the read in
/// (mirroring emitCharRegionArg's strip) plus the load itself.
static const clang::VarDecl *asPointerVarRead(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    if (cast->getCastKind() != clang::CK_BitCast &&
        cast->getCastKind() != clang::CK_NoOp &&
        cast->getCastKind() != clang::CK_LValueToRValue)
      break;
    if (cast->getCastKind() != clang::CK_LValueToRValue &&
        !isPointerType(cast->getType()))
      break;
    e = stripTrivia(cast->getSubExpr());
  }
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
  const auto *var =
      ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
  return var && var->hasLocalStorage() ? var : nullptr;
}

const clang::VarDecl *
CImporter::asMultiBasePointerRead(const clang::Expr *expr) const {
  const clang::VarDecl *var = asPointerVarRead(expr);
  if (!var)
    return nullptr;
  auto it = pointerLocals.find(var);
  return it != pointerLocals.end() && it->second.baseIndexCell ? var
                                                               : nullptr;
}

const clang::VarDecl *
CImporter::asPlainCursorPointerRead(const clang::Expr *expr) const {
  const clang::VarDecl *var = asPointerVarRead(expr);
  if (!var)
    return nullptr;
  auto it = pointerLocals.find(var);
  if (it == pointerLocals.end())
    return nullptr;
  const PointerLocalInfo &info = it->second;
  if (!info.base || !info.cursorCell || info.baseIndexCell ||
      info.nonNullCell || info.member || info.literalBacking || info.backing)
    return nullptr;
  return var;
}

LogicalResult CImporter::emitMultiBaseCallDispatch(
    const clang::CallExpr *call, func::FuncOp target, Location loc,
    MutableArrayRef<Value> arguments,
    ArrayRef<std::pair<unsigned, const clang::Expr *>> borrows,
    unsigned multiIndex, const clang::VarDecl *multiVar) {
  (void)call;
  FunctionType targetType = target.getFunctionType();
  const PointerLocalInfo &info = pointerLocals.find(multiVar)->second;
  Type multiInput = targetType.getInput(multiIndex);
  auto multiSlice =
      llvm::dyn_cast_or_null<emitrust::SliceType>(borrowPointee(multiInput));
  bool multiMut = llvm::isa<emitrust::MutRefType>(multiInput);
  // Only a slice input has a per-base region view; anything else keeps
  // the located multi-base rejection.
  if (!multiSlice)
    return emitError(loc) << "unsupported: passing a pointer bound to "
                             "multiple objects to a function";
  Value discriminant = loadPlace(loc, info.baseIndexCell);
  Value multiCursor =
      info.cursorCell
          ? loadPlace(loc, info.cursorCell)
          : createIntConstant(loc, builder.getIntegerType(64), 0);
  Type ui8Type =
      IntegerType::get(builder.getContext(), 8, IntegerType::Unsigned);
  // The active base's whole-region place + element type. Member bases
  // have no shared cursor coordinate and stay located.
  auto armBasePlace = [&](const PointerBaseKey &armBase)
      -> FailureOr<Value> {
    if (armBase.member)
      return emitError(loc) << "unsupported: passing a pointer bound to "
                               "multiple objects to a function";
    auto it = symbols.find(armBase.var);
    if (it == symbols.end())
      return emitError(loc) << "unsupported: pointer target '"
                            << armBase.var->getName()
                            << "' is not an importable place";
    auto lvalueType =
        llvm::dyn_cast<emitrust::LValueType>(it->second.getType());
    Type elementType;
    if (lvalueType) {
      if (auto arrayType =
              llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType()))
        elementType = arrayType.getElementType();
      else if (auto sliceType = llvm::dyn_cast<emitrust::SliceType>(
                   lvalueType.getValueType()))
        elementType = sliceType.getElementType();
    }
    if (!elementType || elementType != multiSlice.getElementType())
      return emitError(loc) << "unsupported: argument element type does "
                               "not match the slice parameter";
    return it->second;
  };
  return emitMultiBaseDispatch(
      loc, info.multiBases, discriminant,
      [&](const PointerBaseKey &armBase) -> LogicalResult {
        FailureOr<Value> basePlace = armBasePlace(armBase);
        if (failed(basePlace))
          return failure();
        SmallVector<Value> armArguments(arguments.begin(), arguments.end());
        // The split pairing: this arm's base also feeds a MUTABLE
        // cursored slice argument of the same call while the multi-base
        // view is SHARED — the aes `XorWithIv(buf, Iv)` arm once Iv is
        // in buf's region. `split_at_mut` at the mutable cursor keeps
        // both borrows legal; the shared window subscripts strictly
        // below it, so an out-of-window read panics where the C read
        // was into the mutable half (the accepted loud refinement).
        int splitPartner = -1;
        const clang::VarDecl *splitVar = nullptr;
        if (!multiMut && multiSlice.getElementType() == ui8Type) {
          for (auto [index, expr] : borrows) {
            if (index == multiIndex)
              continue;
            const clang::VarDecl *plain = asPlainCursorPointerRead(expr);
            if (!plain)
              continue;
            if (pointerLocals.find(plain)->second.base != armBase.var)
              continue;
            auto mutRef = llvm::dyn_cast<emitrust::MutRefType>(
                targetType.getInput(index));
            auto slice =
                mutRef ? llvm::dyn_cast<emitrust::SliceType>(
                             mutRef.getPointee())
                       : emitrust::SliceType();
            if (!slice || slice.getElementType() != ui8Type)
              continue;
            splitPartner = static_cast<int>(index);
            splitVar = plain;
            break;
          }
        }
        // Aliasing roots held in THIS arm (prefix-overlap semantics,
        // exactly the non-dispatch borrow loop's guard).
        SmallVector<std::pair<const clang::VarDecl *,
                              SmallVector<const clang::FieldDecl *, 2>>,
                    4>
            armRoots;
        armRoots.push_back({armBase.var, {}});
        auto sliceUi8 = emitrust::SliceType::get(ui8Type);
        if (splitPartner >= 0) {
          Value mutCursor = loadPlace(
              loc, pointerLocals.find(splitVar)->second.cursorCell);
          Type mutSliceRef = emitrust::MutRefType::get(sliceUi8);
          Value zero =
              createIntConstant(loc, builder.getIntegerType(64), 0);
          Value whole = builder
                            .create<emitrust::SliceOfOp>(
                                loc, mutSliceRef, *basePlace, zero,
                                /*is_mut=*/true)
                            .getResult();
          requestStringHelper("__emitrust_split_mut_u8");
          auto split = builder.create<emitrust::CallOpaqueOp>(
              loc, TypeRange{mutSliceRef, mutSliceRef},
              builder.getStringAttr("__emitrust_split_mut_u8"),
              /*args=*/ArrayAttr(), ValueRange{whole, mutCursor});
          armArguments[splitPartner] = split.getResult(1); // [cursor..]
          Value lowPlace = builder
                               .create<emitrust::DerefOp>(
                                   loc, emitrust::LValueType::get(sliceUi8),
                                   split.getResult(0))
                               .getResult();
          armArguments[multiIndex] =
              builder
                  .create<emitrust::SliceOfOp>(loc, multiInput, lowPlace,
                                               multiCursor,
                                               /*is_mut=*/false)
                  .getResult();
        } else {
          armArguments[multiIndex] =
              builder
                  .create<emitrust::SliceOfOp>(loc, multiInput, *basePlace,
                                               multiCursor, multiMut)
                  .getResult();
        }
        for (auto [index, expr] : borrows) {
          if (index == multiIndex ||
              static_cast<int>(index) == splitPartner)
            continue;
          const clang::VarDecl *root = nullptr;
          SmallVector<const clang::FieldDecl *, 2> rootPath;
          FailureOr<Value> reference = emitBorrowArgument(
              loc, expr, targetType.getInput(index), root, &rootPath);
          if (failed(reference))
            return failure();
          if (root) {
            for (const auto &held : armRoots) {
              if (held.first != root)
                continue;
              size_t common =
                  std::min(held.second.size(), rootPath.size());
              if (llvm::ArrayRef(held.second).take_front(common) ==
                  llvm::ArrayRef(rootPath).take_front(common))
                return emitError(loc)
                       << "unsupported: aliasing mutable pointer arguments "
                          "(two arguments borrow object '"
                       << root->getName() << "')";
            }
            armRoots.push_back({root, rootPath});
          }
          armArguments[index] = *reference;
        }
        for (auto [index, value] : llvm::enumerate(armArguments))
          if (value.getType() != targetType.getInput(index))
            return emitError(loc)
                   << "unsupported: call argument type mismatch";
        builder.create<func::CallOp>(loc, target, armArguments);
        // Staged byte-region-global slice arguments store back inside
        // the arm — the same load-modify-store shape as the
        // non-dispatch call tail.
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
        return success();
      });
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
  // FR-76: a Ref/MutRef input — a fn_ptr carrying region-typed slice
  // components — takes a borrow argument exactly like a direct call to a
  // slice-classified callee: value arguments materialize first (no load
  // may sit between a `&mut` borrow and the call consuming it), each
  // borrow resolves to a fresh region view through `emitBorrowArgument`,
  // and two borrows of one base keep the same-base aliasing rejection
  // with `emitCall`'s (base, member-path) prefix-overlap semantics. A
  // fn_ptr can never carry a cell-slice input (the component verifier
  // admits only slice refs), so the direct path's cell-global staging has
  // no counterpart here.
  ArrayRef<Type> inputs = fnPtrType.getInputs();
  SmallVector<Value> arguments(call->getNumArgs(), Value());
  struct PendingBorrow {
    unsigned index;
    const clang::Expr *expr;
  };
  SmallVector<PendingBorrow, 4> borrows;
  for (auto [index, argument] : llvm::enumerate(call->arguments())) {
    Type input = index < inputs.size() ? inputs[index] : Type();
    if (input && llvm::isa<emitrust::MutRefType, emitrust::RefType>(input)) {
      borrows.push_back({static_cast<unsigned>(index), argument});
      continue;
    }
    FailureOr<Value> value = emitPositionedRValue(input, argument);
    if (failed(value))
      return failure();
    arguments[index] = *value;
  }
  if (arguments.size() != inputs.size())
    return emitError(loc) << "unsupported: call argument count mismatch";
  SmallVector<std::pair<const clang::VarDecl *,
                        SmallVector<const clang::FieldDecl *, 2>>,
              4>
      borrowRoots;
  for (const PendingBorrow &borrow : borrows) {
    const clang::VarDecl *root = nullptr;
    SmallVector<const clang::FieldDecl *, 2> rootPath;
    FailureOr<Value> reference = emitBorrowArgument(
        loc, borrow.expr, inputs[borrow.index], root, &rootPath);
    if (failed(reference))
      return failure();
    if (root) {
      for (const auto &held : borrowRoots) {
        if (held.first != root)
          continue;
        size_t common = std::min(held.second.size(), rootPath.size());
        if (llvm::ArrayRef(held.second).take_front(common) ==
            llvm::ArrayRef(rootPath).take_front(common))
          return emitError(loc)
                 << "unsupported: aliasing mutable pointer arguments (two "
                    "arguments borrow object '"
                 << root->getName() << "')";
      }
      borrowRoots.push_back({root, rootPath});
    }
    arguments[borrow.index] = *reference;
  }
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
  // parameters) and functions whose pointer parameters classify to a shape
  // the fn_ptr's component does not carry — after FR-76/FR-102 that is no
  // longer every data pointer (an arithmetic-pointee slice and a
  // complete-record `&mut Record` both unify), but CellSlice, Carrier and
  // owner-promoted receivers still land here, loudly.
  FunctionType targetType = target.getFunctionType();
  if (targetType.getInputs() != fnPtrType.getInputs() ||
      targetType.getResults() != fnPtrType.getResults())
    return emitError(loc)
           << "unsupported: function '" << name
           << "' does not match the function pointer signature";
  // FR-77: the emitted constant will spell `Some(<name>)` — opaque text no
  // symbol-table walk can see — so a name NO translation unit defines must
  // be refused HERE, at the address-taking site, where the diagnostic is
  // located on the C construct and the per-decl recovery walk can drop the
  // containing item (and transitively stub its readers) instead of losing
  // the crate to a dangling reference (rustc E0425). The project-wide
  // definition set comes from the pre-import whole-program scan, so the
  // answer is independent of TU import order; defer mode skips the gate —
  // a shard cannot know the link line, so `finalizeProject` marks the
  // prototype as an `emitrust.extern_decl` link obligation instead. The
  // historical single-file entry point (no pre-scan, no finalize) also
  // stays untouched: it emits a body-less prototype exactly as before.
  if (wholeProgram.prescanRan && !deferExternals && !callee->hasBody() &&
      !wholeProgram.definedFunctions.contains(name))
    return emitError(loc)
           << "unsupported: taking the address of undefined function '" << name
           << "'";
  // FR-52: the emitted `Some(<name>)` constant spells the item out directly,
  // so this name can never be routed through the external-requirement trait.
  // Recorded here — the one place a function address resolves — with the
  // first resolution site, the location FR-77's finalize-time refusal needs
  // (an initializer-only reference leaves no SymbolUse to locate on).
  fnPointerTargetSymbols.try_emplace(name, loc);
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
  std::string path = (llvm::Twine(enumTypeRustName(definition->getName())) +
                      "::" + enumVariantRustName(enumerator->getName()))
                         .str();
  auto type = emitrust::EnumType::get(
      builder.getContext(), enumTypeRustName(definition->getName()));
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
  // `(const void *) 0`: only the UNQUALIFIED `void *` cast is a formal
  // null pointer constant (C11 6.3.2.3p3), but the qualified cast still
  // yields the null pointer value; clang models both as CK_NullToPointer.
  // FR-88 extends the same reasoning to the corpus's `(uint8_t *) 0`
  // spelling, whose NullToPointer cast arrives WRAPPED in an implicit
  // BitCast/NoOp qualification adjustment (tinycrypt's `p == (uint8_t *)
  // 0` sanity checks); the shared helper peels the wrapper.
  return isNullPointerConstantShape(expr, astContext());
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


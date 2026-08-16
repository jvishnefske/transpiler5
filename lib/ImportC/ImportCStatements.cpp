//===- ImportCStatements.cpp - statement import -----------------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// CImporter's statement import: emitStmt and every statement-kind handler
/// it dispatches to (emitLocalVar and its aggregate/pointer/compound-literal
/// initializer helpers, emitIfStmt/emitWhileStmt/emitForStmt/emitDoStmt/
/// emitSwitchStmt/emitDispatchSwitch, emitReturnStmt, emitExprStmt,
/// emitAssign, emitIncDec, emitCallStmt), plus the printf/fprintf/sprintf
/// family (emitAliasedPrintf/emitPrintf/translatePrintfFormat/emitSprintf)
/// and the puts/strlen helpers that close out the section — all of it
/// reachable from emitCallStmt's dispatch, and grouped here rather than with
/// the hosted-<stdio.h> FILE* machinery in ImportCHosted.cpp because that is
/// where the original file's own "Statements" section boundary put them.
/// Split out of ImportC.cpp by pure code motion (W1.10); see
/// CImporterInternal.h for the CImporter class declaration this file
/// implements.
//
//===----------------------------------------------------------------------===//

#include "CImporterInternal.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

LogicalResult CImporter::emitStmt(const clang::Stmt *stmt) {
  // Belt-and-suspenders: a body-less function (e.g. an explicitly defaulted
  // special member whose `getBody()` is null) must never reach the statement
  // walk — `importCXXMethods` filters those out — but guard rather than
  // dereference a null `stmt` into a crash if a new path ever slips through.
  if (!stmt)
    return success();
  // FR-64: the constant-fill loop and NUL terminator of a lifted string
  // buffer are fused into its `String::repeat` binding at the decl site, so
  // they emit nothing here. (The `free` call is intercepted in the free
  // handler, not elided.)
  if (stringFillElidedStmts.contains(stmt))
    return success();
  // FR-94: the malloc-failure null guard (`if (!d) ...`) of a claimed
  // owned-tail FAM local is elided — `vec!` is infallible (the FR-65
  // precedent), so the guard body is unreachable in the emitted crate and
  // its `return NULL` has no owned-return representation.
  if (famElidedStmts.contains(stmt))
    return success();
  Location loc = translateLoc(stmt->getBeginLoc());

  if (const auto *compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
    for (const clang::Stmt *child : compound->body())
      if (failed(emitStmt(child)))
        return failure();
    return success();
  }
  if (llvm::isa<clang::NullStmt>(stmt))
    return success();
  if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
    for (const clang::Decl *decl : declStmt->decls()) {
      // W2.9: a structured binding (`auto [a, b] = src;`) has its own
      // desugar; DecompositionDecl IS-A VarDecl, so this must be checked
      // before the generic local-variable path silently mishandles it.
      if (const auto *decomp = llvm::dyn_cast<clang::DecompositionDecl>(decl)) {
        if (failed(emitDecompositionDecl(decomp)))
          return failure();
        continue;
      }
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        // W2.13: `auto f = [caps](params) {...};` lifts to a module-level
        // fn at the declaration point. Intercepted BEFORE the generic
        // local-variable path — emitLocalVar would convert the closure
        // record's type and die at the operator() member-shape gate. The
        // C++17 AST shape is VarDecl cinit -> LambdaExpr DIRECTLY (no
        // construct/cleanups wrapper), and matching only that shape keeps
        // e.g. a lambda-to-fn-pointer conversion initializer (cast nodes
        // in between) on its historical rejection path.
        if (var->hasLocalStorage() && var->getInit())
          if (const auto *lambda = llvm::dyn_cast<clang::LambdaExpr>(
                  var->getInit()->IgnoreParens())) {
            if (failed(emitLambdaLocal(var, lambda)))
              return failure();
            continue;
          }
        if (failed(emitLocalVar(var)))
          return failure();
        continue;
      }
      if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(decl)) {
        if (failed(importRecord(record, translateLoc(record->getBeginLoc()))))
          return failure();
        continue;
      }
      if (const auto *enumDecl = llvm::dyn_cast<clang::EnumDecl>(decl)) {
        if (failed(
                importEnum(enumDecl, translateLoc(enumDecl->getBeginLoc()))))
          return failure();
        continue;
      }
      if (const auto *funcDecl = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        // A block-scope function declaration has external linkage
        // (C11 6.2.2p5), so it is hoisted to module scope and imported
        // through the same path as a file-scope prototype (including the
        // body-less-function check in `finalizeProject`). It is always a
        // prototype: clang rejects nested function definitions before the
        // importer runs. `importFunction` guards the builder's insertion
        // point, so emission resumes in the current block afterwards.
        if (failed(importFunction(funcDecl)))
          return failure();
        continue;
      }
      if (llvm::isa<clang::TypedefDecl>(decl))
        continue;
      return emitError(translateLoc(decl->getBeginLoc()))
             << "unsupported declaration inside a function body";
    }
    return success();
  }
  if (const auto *ret = llvm::dyn_cast<clang::ReturnStmt>(stmt))
    return emitReturnStmt(ret);
  if (const auto *ifStmt = llvm::dyn_cast<clang::IfStmt>(stmt))
    return emitIfStmt(ifStmt);
  if (const auto *whileStmt = llvm::dyn_cast<clang::WhileStmt>(stmt))
    return emitWhileStmt(whileStmt);
  if (const auto *forStmt = llvm::dyn_cast<clang::ForStmt>(stmt))
    return emitForStmt(forStmt);
  // W2.10: ranged-for over a recognized container local.
  if (const auto *rangeFor = llvm::dyn_cast<clang::CXXForRangeStmt>(stmt))
    return emitCXXForRangeStmt(rangeFor);
  if (const auto *switchStmt = llvm::dyn_cast<clang::SwitchStmt>(stmt))
    return emitSwitchStmt(switchStmt);
  if (llvm::isa<clang::BreakStmt>(stmt)) {
    if (loopStack.empty())
      return emitError(loc)
             << "unsupported: 'break' outside of a loop or switch";
    builder.create<cf::BranchOp>(loc, loopStack.back().breakDest);
    builder.setInsertionPointToEnd(createBlock());
    return success();
  }
  if (llvm::isa<clang::ContinueStmt>(stmt)) {
    // A switch inherits the continue target of its enclosing loop; a null
    // target means the innermost switch has no enclosing loop.
    if (loopStack.empty() || !loopStack.back().continueDest)
      return emitError(loc) << "unsupported: 'continue' outside of a loop";
    builder.create<cf::BranchOp>(loc, loopStack.back().continueDest);
    builder.setInsertionPointToEnd(createBlock());
    return success();
  }
  if (const auto *doStmt = llvm::dyn_cast<clang::DoStmt>(stmt))
    return emitDoStmt(doStmt);
  if (llvm::isa<clang::IndirectGotoStmt>(stmt))
    return emitError(loc) << "unsupported: computed goto";
  if (const auto *gotoStmt = llvm::dyn_cast<clang::GotoStmt>(stmt)) {
    builder.create<cf::BranchOp>(loc, getLabelBlock(gotoStmt->getLabel()));
    // Continue in a fresh block; if it stays unreachable it is erased later.
    builder.setInsertionPointToEnd(createBlock());
    return success();
  }
  if (const auto *labelStmt = llvm::dyn_cast<clang::LabelStmt>(stmt)) {
    Block *block = getLabelBlock(labelStmt->getDecl());
    if (!isTerminated(builder.getInsertionBlock()))
      builder.create<cf::BranchOp>(loc, block); // Fall into the label.
    builder.setInsertionPointToEnd(block);
    return emitStmt(labelStmt->getSubStmt());
  }
  if (const auto *switchCase = llvm::dyn_cast<clang::SwitchCase>(stmt)) {
    // Reached only under a dispatch-lowered switch (`emitDispatchSwitch`
    // pre-registers every label of the switch before walking its body; the
    // structured lowering peels its labels itself and never routes them
    // here). The label is an ordinary block boundary: fall into its
    // pre-created dispatch target, exactly like a C label.
    Block *block = switchCaseBlocks.lookup(switchCase);
    if (!block)
      return emitError(loc)
             << "unsupported: case label outside of an enclosing switch";
    if (!isTerminated(builder.getInsertionBlock()))
      builder.create<cf::BranchOp>(loc, block); // Fall into the label.
    builder.setInsertionPointToEnd(block);
    return emitStmt(switchCase->getSubStmt());
  }
  if (const auto *expr = llvm::dyn_cast<clang::Expr>(stmt))
    return emitExprStmt(expr);
  return emitError(loc) << "unsupported statement: "
                        << stmt->getStmtClassName();
}

/// Returns whether any `DeclRefExpr` under `stmt` references `var`.
/// Drives dead-VLA elision (CTS-F, 00207): "unreferenced" means no use
/// anywhere in the function body, including unevaluated contexts such as
/// `sizeof` (whose operand is a child of the trait expression), so any
/// mention at all keeps the existing rejection.
static bool referencesVar(const clang::Stmt *stmt,
                          const clang::VarDecl *var) {
  if (!stmt)
    return false;
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
    if (ref->getDecl()->getCanonicalDecl() == var->getCanonicalDecl())
      return true;
  for (const clang::Stmt *child : stmt->children())
    if (referencesVar(child, var))
      return true;
  return false;
}

LogicalResult CImporter::emitLocalVar(const clang::VarDecl *var) {
  Location loc = translateLoc(var->getLocation());
  // A `va_list` local inside a monomorphization clone (CTS 00204) has no
  // storage of its own: the consumption cursor is the clone's internal
  // cell, and every reference to the object is consumed by the
  // va_start/va_arg/va_end lowerings (the planner verified this).
  // Outside a clone the type keeps its C99-37 rejection below. Both
  // checks go through the typedef SUGAR, not the canonical type: on
  // AArch64 Darwin `__builtin_va_list` is plain `char *`, so a canonical
  // match would swallow every char* clone-local here and miss the
  // rejection below entirely (the pointer machinery diverts before
  // mapType's canonical checks ever run).
  if (isVaListSugarType(var->getType(), astContext())) {
    if (currentVaCloneActive)
      return success();
    return emitError(loc) << "unsupported: va_list type";
  }
  // C99-7: pointer locals divert into the decomposition before `mapType`
  // runs, so the volatile scan happens up front for every local shape
  // (including the pointer's own qualifier, `int * volatile p`).
  if (hasVolatileQualifier(astContext(), var->getType()))
    return emitError(loc) << "unsupported: volatile-qualified type";
  if (!var->hasLocalStorage()) {
    if (var->isStaticLocal()) {
      // A function-local static is module-level state initialized once at
      // program start (its C initializer must be a constant expression).
      // It is mangled as <function>_<name>; createGlobal rejects the
      // mangled name if it collides with an existing module symbol.
      // A function-local static is module-level state, so it takes the global
      // spelling (SCREAMING_SNAKE_CASE under the idiomatic rename).
      std::string mangled = globalRustName(
          (llvm::Twine(currentFuncName) + "_" + var->getName()).str());
      return createGlobal(var->getCanonicalDecl(), var, mangled, loc);
    }
    return emitError(loc) << "unsupported: extern local variable";
  }
  // FR-64: a recognized constant-fill string buffer lifts whole to a
  // `let a: String = "c".repeat(n as usize)` binding, replacing the pointer
  // decomposition entirely (its fill loop, NUL store, and `free` are fused
  // or elided). Checked before every pointer/owner path so the `char *`
  // never reaches the region model.
  if (stringFillLocals.contains(var))
    return emitStringFillLocal(var, loc);
  // FR-65: a recognized runtime-sized heap buffer lifts whole to a
  // `let a: Vec<T> = vec![<zero>; n as usize]` binding, replacing the pointer
  // decomposition entirely (its `a[i]` uses become `Vec` index places and
  // `free` a no-op). Checked before every pointer/owner path so the `T *`
  // never reaches the region model.
  if (vecValueLocals.contains(var))
    return emitVecLocal(var, loc);
  // FR-94: a recognized FAM-record owned-tail local lifts whole to an owned
  // struct binding whose tail member is `vec![0u8; n]` (or the by-value
  // result of a recognized owned-return allocator call), replacing the
  // pointer decomposition entirely.
  if (famAllocLocals.contains(var))
    return emitFamLocal(var, loc);
  // An owner-promoted array (Phase 4) declares the owner struct variable
  // instead; every direct access rewrites to the struct's "data" member.
  if (ownerPlans.contains(var))
    return emitOwnerLocal(var, loc);
  // Dead-VLA elision (CTS-F, 00207): an UNREFERENCED local VLA whose
  // size expression is side-effect-free is elided entirely — no IR, no
  // diagnostic. The object never materializes, and dropping the (pure)
  // size expression loses nothing. A referenced VLA — and a dead one
  // whose size expression has side effects (eliding it would silently
  // lose the effect) — keeps the `unsupported: non-constant array size`
  // rejection `mapType` emits below.
  {
    clang::QualType probe = var->getType();
    bool isVla = false;
    bool sizeSideEffectFree = true;
    while (const clang::ArrayType *array = astContext().getAsArrayType(probe)) {
      if (const auto *vla = llvm::dyn_cast<clang::VariableArrayType>(array)) {
        isVla = true;
        if (vla->getSizeExpr() &&
            vla->getSizeExpr()->HasSideEffects(astContext()))
          sizeSideEffectFree = false;
      }
      probe = array->getElementType();
    }
    if (isVla && sizeSideEffectFree &&
        !referencesVar(currentFunctionBody, var))
      return success();
  }
  // An admitted local `void *` fn-ptr holder (CTS-F, 00210) imports
  // exactly like a directly-typed local fn-ptr; the pointer
  // decomposition never sees it (`fnHolderQuery`).
  if (const clang::FunctionDecl *target = voidFnPtrHolders.lookup(var))
    return emitFnHolderLocal(var, target, loc);
  clang::QualType type = var->getType().getCanonicalType();
  // A FILE* handle local (uninitialized or fopen-initialized) is an owned
  // stream handle over std::fs (C99-48), never a decomposed pointer;
  // other FILE* initializers (stdout, ...) keep the historical pointer
  // path and its located rejections.
  if (isFileHandleLocal(var))
    return emitFileLocal(var, loc);
  // Function pointers are ordinary `!emitrust.fn_ptr` values and take the
  // plain variable path below, bypassing the pointer decomposition.
  if (type->isPointerType() && !type->isFunctionPointerType())
    return emitPointerLocal(var, loc);
  // W2.12: a literal-initialized `std::string_view` local decomposes into
  // (shared literal backing, i64 cursor cell, i64 len cell) — no
  // string_view type is ever materialized, so the divert happens BEFORE
  // `mapType` runs. Any OTHER string_view local shape (from a
  // std::string, from another view, uninitialized, ...) falls through to
  // `mapType`'s located tail rejection below.
  if (isStdStringViewRecordType(type))
    if (const clang::StringLiteral *literal = matchStringViewLiteralInit(var))
      return emitStringViewLocal(var, literal, loc);
  FailureOr<Type> mlirType = mapType(type, loc);
  if (failed(mlirType))
    return failure();
  // A callsite-inferred prototype-less fn-ptr local (FR-29, CTS 00209)
  // declares at its refined signature instead of the zero-parameter
  // no-proto mapping; its initializer binds against the refinement below.
  if (emitrust::FnPtrType refined = inferredFnPtrSigs.lookup(var))
    mlirType = Type(refined);

  bool isAggregate =
      llvm::isa<emitrust::StructType, emitrust::ArrayType>(*mlirType);
  // W2.3: a recognized STL opaque local (`std::vector<T>`/`std::string`)
  // lives in an `emitrust.variable` place exactly like a struct local — a
  // memref of a dialect type is illegal, same as the enum/fn_ptr reason
  // below — and its (always-significant; the default ctor is never
  // trivial) constructor initializer is handled by the dedicated
  // `emitStlConstruct`, not the generic aggregate branch.
  bool isStlOpaque = isStlOpaqueType(*mlirType);
  // Enums, function pointers, and unsigned scalars live in
  // `emitrust.variable` places rather than memref cells: a memref of a
  // dialect type is illegal, and mem2reg materializes an unsigned cell's
  // default value as an `arith.constant`, which requires a signless type.
  // A W2.14 std::variant local (a synthesized `!emitrust.data_enum`) is a
  // place for the same dialect-type reason; its construction is ALWAYS an
  // explicit enum_variant assign below (no init attribute — a data enum
  // deliberately derives no Default, and the emitter's deferred-init path
  // renders the place correctly for both the single-assign and the
  // reassigned shape).
  bool isPlaceOnly =
      llvm::isa<emitrust::EnumType, emitrust::FnPtrType,
                emitrust::DataEnumType>(*mlirType) ||
      isStlOpaque;
  // FR-61f: a signed-scalar local a range-eligible `for` body touches is
  // routed to a place too — a `memref.alloca` cell cannot be promoted by
  // mem2reg across the region op and `convert-to-emitrust` rejects it.
  if (isAggregate || isPlaceOnly || isUnsignedInt(*mlirType) ||
      addressTaken.contains(var) || placeBackedScalars.contains(var)) {
    // FR-61f: a place-backed range-`for` scalar with a compile-time-constant
    // integer initializer carries it as the variable's init attribute, so it
    // renders `let mut s: i32 = 0;` instead of a late `let mut s; s = 0;`
    // (clippy::needless_late_init). Scoped to placeBackedScalars so no other
    // place local's golden shifts.
    if (placeBackedScalars.contains(var) && !isUnsignedInt(*mlirType))
      if (const clang::Expr *init = significantInit(var))
        if (std::optional<llvm::APSInt> constant =
                init->getIntegerConstantExpr(astContext())) {
          Value place = createVariablePlace(
              loc, *mlirType,
              var->getName().empty() ? std::string()
                                     : mangleMemberName(var->getName()),
              builder.getIntegerAttr(*mlirType, constant->getExtValue()));
          symbols[var] = place;
          return success();
        }
    // FR-61e: a decl-bound place carries the local's final Rust spelling.
    Value place = createVariablePlace(
        loc, *mlirType,
        var->getName().empty() ? std::string()
                               : mangleMemberName(var->getName()));
    symbols[var] = place;
    if (const clang::Expr *init = significantInit(var)) {
      // W2.14: a std::variant local's initializer is a CXXConstructExpr
      // (the converting ctor from an alternative value, or the
      // NON-vacuous default ctor — variant's is not trivial, so
      // significantInit keeps it); it routes to emitVariantConstruct and
      // the resulting enum_variant value is assigned into the place.
      if (llvm::isa<emitrust::DataEnumType>(*mlirType)) {
        const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(
            init->IgnoreParenImpCasts());
        if (!construct)
          return emitError(loc) << "unsupported: std::variant initializer";
        FailureOr<Value> value = emitVariantConstruct(*mlirType, construct,
                                                      loc);
        if (failed(value))
          return failure();
        return storeToPlace(loc, place, *value);
      }
      if (isStlOpaque) {
        const clang::Expr *unwrapped = init->IgnoreParenImpCasts();
        const auto *construct =
            llvm::dyn_cast<clang::CXXConstructExpr>(unwrapped);
        if (!construct) {
          // W2.11: a non-CXXConstructExpr initializer of an STL opaque
          // local — `std::optional<int> a = find_even(8);`, a bare
          // CallExpr under C++17's guaranteed elision (no ctor wrapper
          // exists in the AST) — initializes from the loaded rvalue when
          // its mapped type matches exactly; anything else keeps the
          // located rejection.
          FailureOr<Value> value = emitRValue(init);
          if (failed(value))
            return failure();
          if (*value && (*value).getType() == *mlirType)
            return storeToPlace(loc, place, *value);
          return emitError(loc)
                 << "unsupported: std::vector/std::string initializer";
        }
        FailureOr<Value> value = emitStlConstruct(*mlirType, construct, loc);
        if (failed(value))
          return failure();
        return storeToPlace(loc, place, *value);
      }
      if (isAggregate) {
        // CTS-BR (00216): byte-region locals initialize per byte —
        // folded constants at their layout offsets, embedded region
        // copies for struct-value elements and whole-copy initializers,
        // runtime scalars through their AST conversion casts.
        if (isByteRegionAggregate(var->getType()))
          return emitByteRegionInit(place, 0, var->getType(), init);
        // `= {...}` lists and `char s[] = "..."` string initializers are
        // supported; a whole-aggregate copy initializer stays rejected.
        // A compound-literal initializer (`struct S s = (struct S){...}`,
        // C99-13) copies a temp that is immediately dead, so it
        // initializes the variable directly through its own list.
        const clang::Expr *unwrapped = init->IgnoreParenImpCasts();
        if (const auto *compound =
                llvm::dyn_cast<clang::CompoundLiteralExpr>(unwrapped))
          unwrapped = compound->getInitializer()->IgnoreParenImpCasts();
        // W2.7: `std::array<T, N> a = {e0, ...};` — the semantic
        // InitListExpr is STRUCT-shaped (one member, the record's inner
        // `T[N]`), wrapping the element list one level deep. Peel to the
        // inner list so the ordinary array-init path below sees the
        // elements; the mapped type is already `!emitrust.array<NxT>`.
        if (const auto *outer = llvm::dyn_cast<clang::InitListExpr>(unwrapped);
            outer && outer->getNumInits() == 1 &&
            isStdArrayRecordType(var->getType()))
          if (const auto *inner =
                  llvm::dyn_cast<clang::InitListExpr>(outer->getInit(0)))
            unwrapped = inner;
        if (const auto *literal =
                llvm::dyn_cast<clang::StringLiteral>(unwrapped))
          return emitStringArrayInit(place, *mlirType, literal);
        // W2.2: `Counter c(5);` / `Counter c2;` — a non-vacuous C++
        // constructor call (a vacuous default-construct wrapper was
        // already stripped by `significantInit`) — default-constructs the
        // place (already done above, an `emitrust.variable`) and then
        // runs the constructor's body as an ordinary `&mut self` method
        // invoked on `&mut place`, discarding its (void) result.
        if (const auto *construct =
                llvm::dyn_cast<clang::CXXConstructExpr>(unwrapped))
          return emitCXXConstructInit(place, construct, loc);
        // Any NON-list aggregate initializer reaching this point is a
        // whole-value copy of an aggregate rvalue, initialized from one
        // loaded value exactly as the assignment form `x = <expr>;` does:
        // a call returning a struct/array (`struct T x = f();`, CTS 00204),
        // a monomorphized `va_arg(ap, struct T)`, or a C copy-initialization
        // from an existing object (`struct T y = x;`). Compound literals,
        // string initializers, and C++ constructor calls were peeled off
        // above; a genuinely unsupported source rejects, located, inside
        // `emitRValue`.
        if (!llvm::isa<clang::InitListExpr>(unwrapped)) {
          FailureOr<Value> value = emitRValue(unwrapped);
          if (failed(value))
            return failure();
          if ((*value).getType() != *mlirType)
            return emitError(loc)
                   << "unsupported: initializer type does not match the "
                      "variable";
          return storeToPlace(loc, place, *value);
        }
        const auto *list = llvm::cast<clang::InitListExpr>(unwrapped);
        return emitAggregateInitList(place, *mlirType, list, var);
      }
      FailureOr<Value> value = emitPositionedRValue(*mlirType, init);
      if (failed(value))
        return failure();
      return storeToPlace(loc, place, *value);
    }
    return success();
  }

  Value cell = createEntryAlloca(loc, *mlirType);
  symbols[var] = cell;
  if (const clang::Expr *init = significantInit(var)) {
    FailureOr<Value> value = emitRValue(init);
    if (failed(value))
      return failure();
    // FR-61e: preserve the C local's source name on the promoted SSA value.
    // A signed non-address-taken scalar is imported as a rank-0 alloca and
    // promoted to SSA by stock mem2reg, which store-forwards the init value
    // (keeping ITS location) onto every use. Wrapping that value's location
    // in a `NameLoc` therefore rides through promotion to the emitter, which
    // reads it in `assignName` and binds `let <name>` instead of `let vN`.
    //
    // Only a fresh, in-function, single-use computation may carry the name.
    // The stored value can be SHARED and renaming it would misname the real
    // owner: a bare load (`int y = x;` — the value IS x's promoted value), a
    // constant (`int n = 5;` — CSE-mergeable), or a parameter/block-argument
    // (`int y = p;`). Naming only clean computations is the accepted partial
    // outcome; the miss is a `vN`, never a wrong name. A single-use scalar
    // stays inlined by FR-61 (no binding, so the carrier is simply unused).
    if (!var->getName().empty() && carriesLocalName(*value))
      (*value).setLoc(NameLoc::get(
          builder.getStringAttr(mangleMemberName(var->getName())),
          (*value).getLoc()));
    return storeToPlace(loc, cell, *value);
  }
  return success();
}

/// FR-61e freshness guard: whether `value` — the imported initializer of a
/// signed scalar local — is a fresh, in-function computation whose location
/// may be repurposed to carry the local's source name (see the call site).
/// Rejects the values whose location is shared with another binding: a
/// parameter / block argument (no defining op), a constant (CSE-mergeable),
/// and a bare load (its value is the loaded variable's own SSA value). The
/// value is freshly produced here and not yet stored, so it currently has no
/// uses; a genuine multi-use only arises later, at which point it is a `let`
/// binding that legitimately wants the name.
bool CImporter::carriesLocalName(Value value) {
  Operation *def = value.getDefiningOp();
  if (!def)
    return false;
  if (llvm::isa<arith::ConstantOp, emitrust::ConstantOp, memref::LoadOp,
                emitrust::LoadOp>(def))
    return false;
  return true;
}

LogicalResult
CImporter::emitCXXConstructInit(Value place,
                                const clang::CXXConstructExpr *construct,
                                Location loc) {
  const clang::CXXConstructorDecl *ctor = construct->getConstructor();
  if (!ctor || ctor->isCopyOrMoveConstructor())
    return emitError(loc) << "unsupported: copy/move construction";
  // A default construction (0 args) whose constructor is NOT user-provided —
  // an implicit or `= default` default ctor made non-trivial only by in-class
  // member initializers (NSDMIs) — is never imported as a function (the
  // method walk skips defaulted/implicit members). Apply each member
  // initializer to `place` directly: `struct D { int x = 5; }; D d;` assigns
  // d.x = 5. A USER-PROVIDED default ctor (a real body) keeps the imported
  // constructor-call path below.
  if (construct->getNumArgs() == 0 && ctor->isDefaultConstructor() &&
      !ctor->isUserProvided())
    return emitDefaultConstructInit(place, ctor, loc);
  // W2.8: std::pair's two-argument value constructor assigns the two
  // fields directly (no libc++ method is ever imported); everything else
  // about the pair — member access, copies, by-value returns — rides the
  // ordinary synthesized-struct machinery. Checked before the imported-
  // constructor lookup, which could never find a std ctor.
  if (isStdPairRecordType(construct->getType()) &&
      construct->getNumArgs() == 2)
    return emitPairConstructInit(place, construct, loc);
  std::string name = cxxMethodMangledName(ctor);
  func::FuncOp target = functions.lookup(name);
  if (!target)
    return emitError(loc)
           << "unsupported: call to an unimported constructor '" << name
           << "'";
  FunctionType targetType = target.getFunctionType();
  if (construct->getNumArgs() + 1 != targetType.getNumInputs())
    return emitError(loc)
           << "unsupported: constructor argument count mismatch";

  Value addrOf = builder
                     .create<emitrust::AddrOfOp>(loc, targetType.getInput(0),
                                                 place, /*is_mut=*/true)
                     .getResult();
  SmallVector<Value> arguments(targetType.getNumInputs(), Value());
  arguments[0] = addrOf;
  for (auto [index, argExpr] : llvm::enumerate(construct->arguments())) {
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
  return success();
}

LogicalResult
CImporter::emitDefaultConstructInit(Value place,
                                    const clang::CXXConstructorDecl *ctor,
                                    Location loc) {
  // Walk the constructor's member initializers in declaration order. An NSDMI
  // field carries a `CXXDefaultInitExpr` whose value `emitRValue` resolves to
  // the in-class initializer; a member with no initializer is absent from
  // this list and keeps `place`'s default (the aggregate-init implicit-zero
  // tail's counterpart). A member initialized by a non-trivial construction
  // of its own rejects, located, inside `emitRValue` rather than silently
  // defaulting to zero.
  for (const clang::CXXCtorInitializer *init : ctor->inits()) {
    if (!init->isMemberInitializer())
      continue;
    const clang::FieldDecl *field = init->getMember();
    if (!field || field->getName().empty())
      return emitError(loc)
             << "unsupported: default constructor member initializer";
    Location fieldLoc = translateLoc(init->getSourceLocation());
    FailureOr<Type> fieldType = mapType(field->getType(), fieldLoc);
    if (failed(fieldType))
      return failure();
    Value fieldPlace =
        builder
            .create<emitrust::MemberOp>(
                fieldLoc, emitrust::LValueType::get(*fieldType), place,
                builder.getStringAttr(field->getName()))
            .getResult();
    FailureOr<Value> value = emitRValue(init->getInit());
    if (failed(value))
      return failure();
    if ((*value).getType() != *fieldType)
      return emitError(fieldLoc)
             << "unsupported: default constructor member initializer type";
    if (failed(storeToPlace(fieldLoc, fieldPlace, *value)))
      return failure();
  }
  return success();
}

LogicalResult
CImporter::emitDecompositionDecl(const clang::DecompositionDecl *decomp) {
  Location loc = translateLoc(decomp->getLocation());
  // Only the BY-VALUE form (`auto [a, b] = src;`) is supported: the
  // holding object is a copy nothing else can alias, so materializing one
  // scalar local per binding is observably identical (each binding IS a
  // member of the hidden copy; separate locals only differ in address
  // identity, which nothing in the subset can observe). The reference
  // forms (`auto &[a, b]`) alias the SOURCE object — per-binding copies
  // would miscompile writes — and stay rejected.
  if (decomp->getType()->isReferenceType())
    return emitError(loc)
           << "unsupported: structured binding by reference";
  clang::QualType holdingType = decomp->getType().getCanonicalType();
  FailureOr<Type> mapped = mapType(holdingType, loc);
  if (failed(mapped))
    return failure();
  auto structType = llvm::dyn_cast<emitrust::StructType>(*mapped);
  auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(*mapped);
  if (!structType && !arrayType)
    return emitError(loc) << "unsupported: structured binding source type";
  const clang::Expr *init = decomp->getInit();
  if (!init)
    return emitError(loc)
           << "unsupported: structured binding without an initializer";
  // The holding value: emitRValue's CXXConstructExpr trivial-copy path
  // unwraps the copy to its source and loads it whole; a factory-call
  // initializer is a plain struct-returning call. Non-trivial sources
  // reject, located, inside emitRValue.
  FailureOr<Value> holdingValue = emitRValue(init->IgnoreParenImpCasts());
  if (failed(holdingValue))
    return failure();
  if ((*holdingValue).getType() != *mapped)
    return emitError(loc)
           << "unsupported: structured binding initializer type";
  Value holdingPlace = createVariablePlace(loc, *mapped, std::string());
  if (failed(storeToPlace(loc, holdingPlace, *holdingValue)))
    return failure();
  llvm::ArrayRef<clang::BindingDecl *> bindings = decomp->bindings();
  for (auto [index, binding] : llvm::enumerate(bindings)) {
    Location bindingLoc = translateLoc(binding->getLocation());
    Value memberPlace;
    if (arrayType) {
      // std::array (and a C array, should one ever reach here) decomposes
      // element-wise: binding i reads element i.
      if (index >= arrayType.getSize())
        return emitError(bindingLoc)
               << "unsupported: structured binding arity";
      Value indexValue =
          builder.create<arith::ConstantOp>(loc, builder.getIndexAttr(index))
              .getResult();
      memberPlace = builder
                        .create<emitrust::SubscriptOp>(
                            bindingLoc,
                            emitrust::LValueType::get(
                                arrayType.getElementType()),
                            holdingPlace, indexValue)
                        .getResult();
    } else {
      // Struct-shaped sources decompose field-wise, in declaration order.
      // For std::pair the tuple-like protocol's get<0>/get<1> are BY
      // DEFINITION .first/.second, so the zip is exact. For any other
      // record, guard that clang itself bound this binding to the zipped
      // field (a MemberExpr on that FieldDecl): a user type with a custom
      // tuple-like protocol (std::tuple_size + get<i>) binds through get
      // calls instead, and a field-zip desugar of one would miscompile —
      // reject it, located, rather than guess.
      const auto *record = holdingType->getAs<clang::RecordType>();
      const clang::RecordDecl *definition =
          record ? record->getDecl()->getDefinition() : nullptr;
      if (!definition)
        return emitError(bindingLoc)
               << "unsupported: structured binding source type";
      llvm::SmallVector<const clang::FieldDecl *> fields;
      for (const clang::FieldDecl *field : definition->fields())
        fields.push_back(field);
      if (fields.size() != bindings.size())
        return emitError(bindingLoc)
               << "unsupported: structured binding arity";
      const clang::FieldDecl *field = fields[index];
      if (!isStdPairRecordType(holdingType)) {
        const auto *bound = llvm::dyn_cast_if_present<clang::MemberExpr>(
            binding->getBinding());
        if (!bound || bound->getMemberDecl() != field)
          return emitError(bindingLoc)
                 << "unsupported: structured binding over a tuple-like "
                    "protocol type";
      }
      if (field->getName().empty())
        return emitError(bindingLoc)
               << "unsupported: structured binding over an unnamed field";
      FailureOr<Type> fieldType = mapType(field->getType(), bindingLoc);
      if (failed(fieldType))
        return failure();
      memberPlace = builder
                        .create<emitrust::MemberOp>(
                            bindingLoc,
                            emitrust::LValueType::get(*fieldType),
                            holdingPlace,
                            builder.getStringAttr(field->getName()))
                        .getResult();
    }
    Value value = loadPlace(bindingLoc, memberPlace);
    Value bindingPlace = createVariablePlace(
        bindingLoc, value.getType(),
        binding->getName().empty()
            ? std::string()
            : mangleMemberName(binding->getName()));
    if (failed(storeToPlace(bindingLoc, bindingPlace, value)))
      return failure();
    symbols[binding] = bindingPlace;
  }
  return success();
}

/// W2.13: returns the first use of `var` under `stmt` that is NOT the
/// callee-object of a direct `operator()` call, or null if every use is
/// one. The callee-object position of a recognized call (`f(args)`: the
/// CXXOperatorCallExpr's arg 0, a DeclRefExpr behind an implicit NoOp
/// const cast) is skipped; its ARGUMENT subtrees are still scanned (a
/// pathological `f(g(1))` must still flag `g`... and even `f(f(1))`'s
/// inner call is itself a recognized callee-object). Any other
/// DeclRefExpr to `var` — a copy initializer, a call argument, a return —
/// is the escape the lift cannot represent.
static const clang::DeclRefExpr *
findNonCallLambdaUse(const clang::Stmt *stmt, const clang::VarDecl *var) {
  if (!stmt)
    return nullptr;
  if (const auto *opCall = llvm::dyn_cast<clang::CXXOperatorCallExpr>(stmt);
      opCall && opCall->getOperator() == clang::OO_Call &&
      opCall->getNumArgs() >= 1) {
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
            opCall->getArg(0)->IgnoreParenImpCasts());
        ref && ref->getDecl()->getCanonicalDecl() == var->getCanonicalDecl()) {
      for (unsigned index = 1; index < opCall->getNumArgs(); ++index)
        if (const clang::DeclRefExpr *bad =
                findNonCallLambdaUse(opCall->getArg(index), var))
          return bad;
      return nullptr;
    }
  }
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
    if (ref->getDecl()->getCanonicalDecl() == var->getCanonicalDecl())
      return ref;
  for (const clang::Stmt *child : stmt->children())
    if (const clang::DeclRefExpr *bad = findNonCallLambdaUse(child, var))
      return bad;
  return nullptr;
}

LogicalResult CImporter::emitLambdaLocal(const clang::VarDecl *var,
                                         const clang::LambdaExpr *lambda) {
  Location loc = translateLoc(var->getLocation());
  // Recognizer gate (design.md W2.13), each failure a located rejection.
  // A default capture ([=]/[&]) captures an implicit, use-derived set;
  // only an EXPLICIT by-value capture list is recognized.
  if (lambda->getCaptureDefault() != clang::LCD_None)
    return emitError(loc) << "unsupported: lambda default capture; only "
                             "explicit by-value captures are supported";
  // A generic lambda's operator() is a template with no one signature to
  // lift.
  if (lambda->getLambdaClass()->isGenericLambda())
    return emitError(loc) << "unsupported: generic lambda";
  // A mutable lambda's operator() can write its closure copy — state the
  // freeze-at-declaration model has no representation for.
  if (lambda->isMutable())
    return emitError(loc) << "unsupported: mutable lambda";
  SmallVector<const clang::VarDecl *, 4> captures;
  for (const clang::LambdaCapture &capture : lambda->captures()) {
    if (capture.capturesThis())
      return emitError(loc) << "unsupported: lambda capture of 'this'";
    if (capture.getCaptureKind() != clang::LCK_ByCopy)
      return emitError(loc) << "unsupported: lambda capture by reference";
    const auto *capVar =
        llvm::dyn_cast_or_null<clang::VarDecl>(capture.getCapturedVar());
    if (!capVar || capVar->isInitCapture())
      return emitError(loc) << "unsupported: lambda init-capture";
    // SCALAR captures only: each freezes to one loaded value passed as a
    // prepended argument. An aggregate would need a by-value copy of the
    // whole object at the declaration point.
    clang::QualType type = capVar->getType();
    if (!type->isIntegerType() && !type->isRealFloatingType())
      return emitError(loc)
             << "unsupported: lambda capture of a non-scalar variable";
    captures.push_back(capVar);
  }
  // Every use of the lambda local must be the callee-object of a direct
  // operator() call: the call rewrite is the ONLY consumer that knows the
  // lifted symbol and the frozen values, so any other use (a copy, an
  // argument, a return) would let the closure escape it.
  if (const clang::DeclRefExpr *bad =
          findNonCallLambdaUse(currentFunctionBody, var))
    return emitError(translateLoc(bad->getBeginLoc()))
           << "unsupported: lambda '" << var->getName()
           << "' escapes its declaration (every use must be a direct call)";
  // Freeze the captures at the declaration point: one load per capture,
  // in capture-list order. This IS C++'s capture-by-value semantics — the
  // closure copies each captured value at construction — so a mutation of
  // the source variable between here and a call is invisible to the call
  // (the EndToEnd byte-diff pins this against the native build).
  SmallVector<Value, 4> frozenCaptures;
  for (const clang::Expr *init : lambda->capture_inits()) {
    FailureOr<Value> value = emitRValue(init);
    if (failed(value))
      return failure();
    frozenCaptures.push_back(*value);
  }
  return importLiftedLambda(var, lambda, std::move(frozenCaptures),
                            std::move(captures), loc);
}

LogicalResult
CImporter::emitPairConstructInit(Value place,
                                 const clang::CXXConstructExpr *construct,
                                 Location loc) {
  // Walk the specialization's fields (exactly `first`, `second`, in
  // declaration order) zipped with the two constructor arguments,
  // mirroring emitDefaultConstructInit's member-place/assign shape. An
  // argument whose value cannot import rejects, located, inside
  // emitRValue.
  const auto *record =
      construct->getType().getCanonicalType()->getAs<clang::RecordType>();
  const clang::RecordDecl *definition = record->getDecl()->getDefinition();
  if (!definition)
    return emitError(loc) << "unsupported: std::pair without a definition";
  unsigned index = 0;
  for (const clang::FieldDecl *field : definition->fields()) {
    if (index >= construct->getNumArgs())
      break;
    if (field->getName().empty())
      return emitError(loc) << "unsupported: std::pair field shape";
    Location fieldLoc = translateLoc(construct->getArg(index)->getBeginLoc());
    FailureOr<Type> fieldType = mapType(field->getType(), fieldLoc);
    if (failed(fieldType))
      return failure();
    Value fieldPlace =
        builder
            .create<emitrust::MemberOp>(
                fieldLoc, emitrust::LValueType::get(*fieldType), place,
                builder.getStringAttr(field->getName()))
            .getResult();
    FailureOr<Value> value = emitRValue(construct->getArg(index));
    if (failed(value))
      return failure();
    if ((*value).getType() != *fieldType)
      return emitError(fieldLoc)
             << "unsupported: std::pair constructor argument type";
    if (failed(storeToPlace(fieldLoc, fieldPlace, *value)))
      return failure();
    ++index;
  }
  if (index != construct->getNumArgs())
    return emitError(loc) << "unsupported: std::pair constructor arity";
  return success();
}

FailureOr<Value>
CImporter::emitStlConstruct(Type stlType,
                            const clang::CXXConstructExpr *construct,
                            Location loc) {
  auto opaque = llvm::cast<emitrust::OpaqueType>(stlType);
  const clang::CXXConstructorDecl *ctor = construct->getConstructor();
  if (ctor && ctor->isCopyOrMoveConstructor())
    return emitError(loc)
           << "unsupported: std::vector/std::string copy/move construction";
  // W2.11: std::optional construction — branched on the spelling BEFORE the
  // zero-argument String/Vec branch below, which must never see Option.
  // Recognized shapes: the default ctor and the std::nullopt_t converting
  // ctor (both render `None`), and the element converting ctor
  // (`Some(v)`). libstdc++/libc++ declare no defaulted trailing parameters
  // on these, but a CXXDefaultArgExpr-filled trailing argument is treated
  // as absent for shape classification, mirroring the String literal ctor.
  if (opaque.getValue().starts_with("Option<")) {
    auto emitNone = [&]() -> FailureOr<Value> {
      return builder
          .create<emitrust::LiteralOp>(loc, stlType,
                                       builder.getStringAttr("None"))
          .getResult();
    };
    llvm::SmallVector<const clang::Expr *> realArgs;
    for (const clang::Expr *arg : construct->arguments())
      if (!llvm::isa<clang::CXXDefaultArgExpr>(arg))
        realArgs.push_back(arg);
    // (i) `std::optional<T> o;` / (ii) all-defaulted arguments -> None.
    if (realArgs.empty())
      return emitNone();
    // (iii) the std::nullopt_t converting ctor -> None.
    const clang::Expr *first = realArgs.front()->IgnoreParenImpCasts();
    if (const auto *record =
            first->getType().getCanonicalType()->getAs<clang::RecordType>();
        record && record->getDecl()->isInStdNamespace() &&
        record->getDecl()->getIdentifier() &&
        record->getDecl()->getName() == "nullopt_t")
      return emitNone();
    // (iv) the element converting ctor -> Some(v). The inner spelling
    // round-trips through parseStlElementType so the argument's mapped
    // type must equal the element type exactly (no implicit conversion
    // surface beyond what clang already materialized in the AST).
    if (realArgs.size() == 1) {
      llvm::StringRef spelling = opaque.getValue();
      Type elementType =
          parseStlElementType(spelling.substr(7, spelling.size() - 8));
      FailureOr<Value> value = emitRValue(realArgs.front());
      if (failed(value))
        return failure();
      if (elementType && (*value).getType() == elementType)
        return builder
            .create<emitrust::CallOpaqueOp>(loc, TypeRange{stlType},
                                            builder.getStringAttr("Some"),
                                            /*args=*/ArrayAttr(),
                                            ValueRange{*value})
            .getResult(0);
    }
    // (v) anything else (in-place construction, converting from another
    // optional's element set, ...) stays a located rejection.
    return emitError(loc)
           << "unsupported: this std::optional constructor shape is not "
              "supported";
  }
  // Zero-argument construction: `std::vector<T> v;` / `std::string s;` (an
  // ALWAYS-significant initializer, unlike a POD struct's vacuous default
  // ctor — see `isVacuousDefaultConstruct` — since neither's default ctor
  // is trivial).
  if (construct->getNumArgs() == 0) {
    llvm::StringRef callee = opaque.getValue() == "String" ? "String::new"
                                                           : "Vec::new";
    return builder
        .create<emitrust::CallOpaqueOp>(loc, TypeRange{stlType},
                                        builder.getStringAttr(callee),
                                        /*args=*/ArrayAttr(), ValueRange{})
        .getResult(0);
  }
  // `std::string s = "literal";` — the single-argument `const char*`
  // conversion constructor over an ordinary string literal (after its
  // array-to-pointer decay). libstdc++'s converting constructor also
  // declares a defaulted allocator parameter (`basic_string(const char*,
  // const Allocator& = Allocator())`), which a `CXXConstructExpr` for an
  // unwritten default argument always fills with a `CXXDefaultArgExpr` —
  // present here even though the user wrote only one argument — so every
  // argument PAST the first must be exactly that, not merely absent. Every
  // other single- or multi-argument construction (the sized/fill vector
  // constructor `std::vector<T>(n)`, an initializer-list constructor, a
  // `std::string` from a `char*` variable, ...) is a located rejection
  // this wave (design.md's STL OUT list).
  bool restAreDefaulted =
      llvm::all_of(llvm::drop_begin(construct->arguments()),
                  [](const clang::Expr *arg) {
                    return llvm::isa<clang::CXXDefaultArgExpr>(arg);
                  });
  if (opaque.getValue() == "String" && construct->getNumArgs() >= 1 &&
      restAreDefaulted) {
    const clang::Expr *arg = construct->getArg(0)->IgnoreParenImpCasts();
    if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(arg)) {
      if (!literal->isOrdinary())
        return emitError(loc) << "unsupported: non-ordinary string literal "
                                 "in std::string construction";
      FailureOr<Value> text = emitRustStrLiteral(loc, literal->getString(),
                                                 "std::string construction");
      if (failed(text))
        return failure();
      return builder
          .create<emitrust::CallOpaqueOp>(loc, TypeRange{stlType},
                                          builder.getStringAttr("String::from"),
                                          /*args=*/ArrayAttr(), ValueRange{*text})
          .getResult(0);
    }
  }
  return emitError(loc)
         << "unsupported: this std::vector/std::string constructor shape "
            "is not supported (only default construction and "
            "std::string's string-literal conversion constructor are "
            "recognized)";
}

std::optional<unsigned>
CImporter::variantAltIndex(emitrust::DataEnumType enumType, Type altType) {
  auto it = variantEnumAlternatives.find(enumType.getName());
  if (it == variantEnumAlternatives.end())
    return std::nullopt;
  for (auto [index, type] : llvm::enumerate(it->second))
    if (type == altType)
      return static_cast<unsigned>(index);
  return std::nullopt;
}

Value CImporter::createVariantValue(Location loc,
                                    emitrust::DataEnumType enumType,
                                    unsigned index, Value payload) {
  return builder
      .create<emitrust::EnumVariantOp>(
          loc, enumType,
          FlatSymbolRefAttr::get(builder.getContext(), enumType.getName()),
          builder.getStringAttr(index == 0 ? "V0" : "V1"),
          ValueRange{payload})
      .getResult();
}

FailureOr<Value>
CImporter::emitVariantConstruct(Type variantType,
                                const clang::CXXConstructExpr *construct,
                                Location loc) {
  auto enumType = llvm::cast<emitrust::DataEnumType>(variantType);
  const clang::CXXConstructorDecl *ctor = construct->getConstructor();
  if (ctor && ctor->isCopyOrMoveConstructor())
    return emitError(loc)
           << "unsupported: std::variant copy/move construction";
  llvm::SmallVector<const clang::Expr *> realArgs;
  for (const clang::Expr *arg : construct->arguments())
    if (!llvm::isa<clang::CXXDefaultArgExpr>(arg))
      realArgs.push_back(arg);
  // (i) `std::variant<A, B> v;` — C++17 [variant.ctor]p2
  // value-initializes the FIRST alternative, so the image is the
  // EXPLICIT `V0 { 0 }` (never the emitter's default-value path: a data
  // enum deliberately derives no Default).
  if (realArgs.empty()) {
    llvm::SmallVector<Type, 2> alternatives =
        variantEnumAlternatives.lookup(enumType.getName());
    Value zero =
        builder
            .create<arith::ConstantOp>(
                loc, llvm::cast<TypedAttr>(builder.getZeroAttr(
                         alternatives[0])))
            .getResult();
    return createVariantValue(loc, enumType, 0, zero);
  }
  // (ii) the converting ctor from an alternative VALUE: the argument's
  // mapped type selects the variant by EXACT type equality (clang
  // already materialized any implicit conversion in the AST).
  if (realArgs.size() == 1) {
    FailureOr<Value> value = emitRValue(realArgs.front());
    if (failed(value))
      return failure();
    if (std::optional<unsigned> index =
            variantAltIndex(enumType, (*value).getType()))
      return createVariantValue(loc, enumType, *index, *value);
  }
  // (iii) anything else (in_place construction, a converting argument
  // outside the alternative set, ...) stays a located rejection.
  return emitError(loc)
         << "unsupported: this std::variant constructor shape is not "
            "supported";
}

void CImporter::collectVoidFnPtrHolders(const clang::Stmt *body) {
  voidFnPtrHolders.clear();
  if (!body)
    return;

  // Candidate pass: a local `void *` initialized with (an implicit cast
  // of) `&f` or the decayed `f` for a known non-variadic function.
  llvm::DenseMap<const clang::VarDecl *, const clang::FunctionDecl *>
      candidates;
  auto collectCandidates = [&](auto &&self, const clang::Stmt *stmt) -> void {
    if (!stmt)
      return;
    if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt))
      for (const clang::Decl *decl : declStmt->decls())
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
          clang::QualType type = var->getType().getCanonicalType();
          if (!var->hasLocalStorage() || llvm::isa<clang::ParmVarDecl>(var) ||
              !type->isPointerType() ||
              !type->getPointeeType()->isVoidType() || !var->getInit())
            continue;
          const clang::Expr *init = var->getInit()->IgnoreParenImpCasts();
          if (const auto *addrOf = llvm::dyn_cast<clang::UnaryOperator>(init))
            if (addrOf->getOpcode() == clang::UO_AddrOf)
              init = addrOf->getSubExpr()->IgnoreParenImpCasts();
          const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(init);
          const auto *target =
              ref ? llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl())
                  : nullptr;
          // A prototype-less K&R target (the 00210 `int f()` shape) is
          // admitted like a directly-typed K&R fn-ptr local: it maps to
          // the zero-parameter form, and the emission's signature check
          // (`resolveFunctionPointerDecl`) still guards the binding.
          if (target && !target->isVariadic())
            candidates[var] = target;
        }
    for (const clang::Stmt *child : stmt->children())
      self(self, child);
  };
  collectCandidates(collectCandidates, body);
  if (candidates.empty())
    return;

  // Consumption pass: mark every holder read that is an explicit cast to
  // EXACTLY the target's signature in callee position. Attributes inside
  // the cast type were already discarded by clang, so the canonical-type
  // comparison sees the plain signature.
  llvm::SmallPtrSet<const clang::DeclRefExpr *, 8> consumed;
  auto consumeCastCalls = [&](auto &&self, const clang::Stmt *stmt) -> void {
    if (!stmt)
      return;
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt))
      if (const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(
              call->getCallee()->IgnoreParens())) {
        const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
            cast->getSubExpr()->IgnoreParenImpCasts());
        const auto *var =
            ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
        auto candidate = var ? candidates.find(var) : candidates.end();
        if (candidate != candidates.end()) {
          clang::QualType castType = cast->getType().getCanonicalType();
          // "Exactly f's signature" is C type compatibility (C11
          // 6.2.7): it equates the cast's prototype with a
          // prototype-less target declaration (the 00210 shape) while
          // rejecting any diverging parameter or result spelling.
          if (castType->isFunctionPointerType() &&
              astContext().typesAreCompatible(
                  castType->getPointeeType(),
                  candidate->second->getType()))
            consumed.insert(ref);
        }
      }
    for (const clang::Stmt *child : stmt->children())
      self(self, child);
  };
  consumeCastCalls(consumeCastCalls, body);

  // Disqualification pass: any other mention of the holder — a
  // reassignment's left-hand side, an escaping argument, a mismatched
  // cast, its address taken — keeps the existing pointer-region
  // rejection.
  auto disqualify = [&](auto &&self, const clang::Stmt *stmt) -> void {
    if (!stmt)
      return;
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
      if (!consumed.contains(ref))
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
          candidates.erase(var);
    for (const clang::Stmt *child : stmt->children())
      self(self, child);
  };
  disqualify(disqualify, body);

  for (const auto &[var, target] : candidates)
    voidFnPtrHolders.try_emplace(var, target);
}

LogicalResult CImporter::inferNoProtoCallSignatures(
    const clang::FunctionDecl *definition,
    llvm::DenseMap<const clang::VarDecl *, emitrust::FnPtrType> &inferred) {
  auto walk = [&](auto &&self, const clang::Stmt *stmt) -> LogicalResult {
    if (!stmt)
      return success();
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
      const clang::Expr *calleeExpr = call->getCallee()->IgnoreParens();
      // `(*fp)(...)`: the decay/deref pair cancels out (see
      // emitIndirectCall).
      if (const auto *decay =
              llvm::dyn_cast<clang::ImplicitCastExpr>(calleeExpr))
        if (decay->getCastKind() == clang::CK_FunctionToPointerDecay) {
          const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(
              decay->getSubExpr()->IgnoreParens());
          if (deref && deref->getOpcode() == clang::UO_Deref &&
              isFunctionPointer(deref->getSubExpr()->getType()))
            calleeExpr = deref->getSubExpr()->IgnoreParens();
        }
      const clang::FunctionNoProtoType *noProto = nullptr;
      const clang::VarDecl *var = nullptr;
      if (call->getNumArgs() > 0 && isFunctionPointer(calleeExpr->getType())) {
        noProto = llvm::dyn_cast<clang::FunctionNoProtoType>(
            calleeExpr->getType()
                .getCanonicalType()
                ->getPointeeType()
                .getTypePtr());
        const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
            calleeExpr->IgnoreParenImpCasts());
        const auto *decl =
            ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
        // Only local-storage decls (parameters and locals): a refined
        // type is a per-function fact, and a global's module-level type
        // must not depend on one body's call sites.
        if (decl && decl->hasLocalStorage())
          var = decl;
      }
      if (noProto && var) {
        Location loc = translateLoc(call->getBeginLoc());
        // Clang already applied the default argument promotions to the
        // arguments of a call through a no-proto type (C11 6.5.2.2p6),
        // so the promoted argument types are used verbatim.
        SmallVector<Type> inputs;
        for (const clang::Expr *argument : call->arguments()) {
          FailureOr<Type> mapped = mapType(argument->getType(), loc);
          if (failed(mapped))
            return failure();
          if (!emitrust::FnPtrType::isValidComponentType(*mapped))
            return emitError(loc)
                   << "unsupported: function pointer parameter type";
          inputs.push_back(*mapped);
        }
        SmallVector<Type> results;
        clang::QualType returnType = noProto->getReturnType();
        if (!returnType->isVoidType()) {
          FailureOr<Type> mapped = mapType(returnType, loc);
          if (failed(mapped))
            return failure();
          if (!emitrust::FnPtrType::isValidComponentType(*mapped))
            return emitError(loc)
                   << "unsupported: function pointer result type";
          results.push_back(*mapped);
        }
        auto signature =
            emitrust::FnPtrType::get(builder.getContext(), inputs, results);
        auto [existing, isNew] = inferred.try_emplace(var, signature);
        if (!isNew && existing->second != signature)
          return emitError(loc)
                 << "unsupported: conflicting inferred prototypes for "
                    "function pointer '"
                 << var->getName() << "'";
      }
    }
    for (const clang::Stmt *child : stmt->children())
      if (failed(self(self, child)))
        return failure();
    return success();
  };
  return walk(walk, definition->getBody());
}

LogicalResult CImporter::emitFnHolderLocal(const clang::VarDecl *var,
                                           const clang::FunctionDecl *target,
                                           Location loc) {
  FailureOr<Type> mapped =
      mapType(astContext().getPointerType(target->getType()), loc);
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
  // FR-61e: the fn-ptr local's place carries the local's final spelling.
  Value place = createVariablePlace(
      loc, fnPtrType,
      var->getName().empty() ? std::string()
                             : mangleMemberName(var->getName()));
  symbols[var] = place;
  auto some = emitrust::OpaqueAttr::get(
      builder.getContext(), (llvm::Twine("Some(") + *name + ")").str());
  Value constant =
      builder.create<emitrust::ConstantOp>(loc, fnPtrType, some).getResult();
  return storeToPlace(loc, place, constant);
}

LogicalResult CImporter::emitOwnerLocal(const clang::VarDecl *var,
                                        Location loc) {
  OwnerPlan &plan = ownerPlans.find(var)->second;
  FailureOr<Type> ownedType = mapType(var->getType(), loc);
  if (failed(ownedType))
    return failure();

  // Synthesize the module-level owner struct on first need; the name is
  // derived from C spellings, so a collision with any existing module
  // symbol is a located rejection (mirroring createGlobal).
  if (!plan.structDefCreated) {
    if (SymbolTable::lookupSymbolIn(module, plan.structName))
      return emitError(loc)
             << "unsupported: owner struct name '" << plan.structName
             << "' collides with an existing symbol";
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::StructDefOp>(
        loc, moduleBuilder.getStringAttr(plan.structName),
        moduleBuilder.getStrArrayAttr(llvm::StringRef("data")),
        moduleBuilder.getTypeArrayAttr(*ownedType));
    plan.structDefCreated = true;
  }

  auto ownerStructType =
      emitrust::StructType::get(builder.getContext(), plan.structName);
  // FR-61e: the owner place is named after the C array variable it owns.
  Value ownerPlace = createVariablePlace(
      loc, ownerStructType,
      var->getName().empty() ? std::string()
                             : mangleMemberName(var->getName()));
  ownerStructPlaces[var] = ownerPlace;
  // Every direct access to the array — and every decomposed pointer whose
  // region base it is — routes through the data member place registered
  // here. Nothing ever loads the owner struct whole: the struct place is
  // only borrowed at method call sites (C arrays are not assignable, so no
  // syntax reaches a whole-owner load).
  Value dataPlace = builder
                        .create<emitrust::MemberOp>(
                            loc, emitrust::LValueType::get(*ownedType),
                            ownerPlace, builder.getStringAttr("data"))
                        .getResult();
  symbols[var] = dataPlace;
  if (const clang::Expr *init = var->getInit()) {
    // The owned array's initializer (a list or a `char s[] = "..."`
    // string) assigns through the data member place, exactly like a plain
    // local array's.
    if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(
            init->IgnoreParenImpCasts()))
      return emitStringArrayInit(dataPlace, *ownedType, literal);
    const auto *list = llvm::dyn_cast<clang::InitListExpr>(init);
    if (!list)
      return emitError(loc) << "unsupported: aggregate initializer";
    return emitAggregateInitList(dataPlace, *ownedType, list);
  }
  return success();
}

LogicalResult
CImporter::emitAggregateInitList(Value place, Type type,
                                 const clang::InitListExpr *list,
                                 const clang::VarDecl *instance) {
  // Sema's semantic form has designators resolved to positional elements
  // and ImplicitValueInitExpr holes for everything left implicit.
  if (const clang::InitListExpr *semantic = list->getSemanticForm())
    list = semantic;
  Location loc = translateLoc(list->getBeginLoc());
  if (auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(type)) {
    if (list->getNumInits() > arrayType.getSize()) // Defensive; Sema rejects.
      return emitError(loc)
             << "unsupported: excess elements in aggregate initializer";
    for (unsigned i = 0, n = list->getNumInits(); i != n; ++i) {
      const clang::Expr *element = list->getInit(i);
      // A hole keeps the place's default element value (C99 zero-fill).
      if (llvm::isa<clang::ImplicitValueInitExpr>(element))
        continue;
      Location elementLoc = translateLoc(element->getBeginLoc());
      Value index = createIntConstant(elementLoc, builder.getIntegerType(64),
                                      static_cast<int64_t>(i));
      Value elementPlace =
          builder
              .create<emitrust::SubscriptOp>(
                  elementLoc,
                  emitrust::LValueType::get(arrayType.getElementType()),
                  place, index)
              .getResult();
      if (failed(emitInitListElement(elementPlace,
                                     arrayType.getElementType(), element)))
        return failure();
    }
    return success();
  }
  if (llvm::isa<emitrust::StructType>(type)) {
    const clang::RecordDecl *record = list->getType()->getAsRecordDecl();
    if (!record) // Defensive; a struct-typed list always has a record.
      return emitError(loc) << "unsupported: aggregate initializer";
return emitRecordInitFields(place, record, list, instance);
  }
  return emitError(loc) << "unsupported: aggregate initializer";
}

LogicalResult
CImporter::emitRecordInitFields(Value place, const clang::RecordDecl *record,
                                const clang::InitListExpr *list,
                                const clang::VarDecl *instance) {
  // Nested lists reached through anonymous members arrive directly (not
  // via emitAggregateInitList), so normalize to the semantic form here
  // too; it is a no-op for a list that already is one.
  if (const clang::InitListExpr *semantic = list->getSemanticForm())
    list = semantic;
  if (record->isUnion()) {
    // A flattened anonymous union member: Sema records the single arm the
    // list initializes; its value lands on the aliased storage slot. A
    // list initializing no arm leaves the slot's default (zero) value.
    const clang::FieldDecl *active = list->getInitializedFieldInUnion();
    if (!active || list->getNumInits() == 0)
      return success();
    return emitRecordInitField(place, active, list->getInit(0), instance);
  }
  unsigned index = 0;
  for (const clang::FieldDecl *field : record->fields()) {
    if (index >= list->getNumInits())
      break; // Remaining fields keep their default (zero) value.
    const clang::Expr *element = list->getInit(index++);
    if (!element || llvm::isa<clang::ImplicitValueInitExpr>(element) ||
        llvm::isa<clang::NoInitExpr>(element))
      continue;
    if (failed(emitRecordInitField(place, field, element, instance)))
      return failure();
  }
  return success();
}

LogicalResult CImporter::emitRecordInitField(Value place,
                                             const clang::FieldDecl *field,
                                             const clang::Expr *element,
                                             const clang::VarDecl *instance) {
  Location elementLoc = translateLoc(element->getBeginLoc());
  // A bit-field member has no field of its own in the flattened
  // struct_def (its storage is a window of a `__bits<n>` backing field);
  // aggregate initialization of one stays out of the C99-45 scope.
  if (field->isBitField())
    return emitError(elementLoc)
           << "unsupported: aggregate initializer for a bit-field member";
  // A flexible array member has no storage behind sizeof on a local
  // object; a GNU zero-length member has no elements, so its (empty)
  // brace initializer emits nothing (CTS-BR, 00216).
  if (field->getType()->isIncompleteArrayType())
    return emitError(elementLoc) << "unsupported: flexible array member access";
  if (isZeroLengthArrayType(field->getType()))
    return success();
  // FR-78: a braced initializer naming an opaque-union arm cannot land on
  // the blob — this per-field path is EXACTLY where the rejected
  // placeholder attempt leaked (the union branch of
  // `emitRecordInitFields` emitted the active arm's member op with no
  // slot lookup, rustc E0609). A list initializing NO arm never reaches
  // here and keeps the blob's zero default.
  if (opaqueUnionArms.contains(field))
    return emitError(elementLoc)
           << "unsupported: opaque union arm initializer";
  // An admitted `void *` fn-ptr member (CTS-BR, 00216) initializes from
  // the address of a function of its one signature: the member place is
  // the retyped fn_ptr field, the value the folded Some(target) (or None
  // for the null constant).
  if (clang::QualType retyped = fnPtrMemberTypes.lookup(field);
      !retyped.isNull()) {
    FailureOr<Type> fieldType = mapType(retyped, elementLoc);
    if (failed(fieldType))
      return failure();
    auto fnPtrType = llvm::dyn_cast<emitrust::FnPtrType>(*fieldType);
    if (!fnPtrType)
      return emitError(elementLoc) << "unsupported function pointer type";
    Value fieldPlace =
        builder
            .create<emitrust::MemberOp>(
                elementLoc, emitrust::LValueType::get(fnPtrType), place,
                builder.getStringAttr(flattenedFieldName(field)))
            .getResult();
    if (element->isNullPointerConstant(astContext(),
                                       clang::Expr::NPC_NeverValueDependent) !=
        clang::Expr::NPCK_NotNull) {
      Value none = builder
                       .create<emitrust::ConstantOp>(
                           elementLoc, fnPtrType,
                           emitrust::OpaqueAttr::get(builder.getContext(),
                                                     "None"))
                       .getResult();
      builder.create<emitrust::AssignOp>(elementLoc, fieldPlace, none);
      return success();
    }
    const clang::Expr *target = element->IgnoreParenCasts();
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(target))
      if (unary->getOpcode() == clang::UO_AddrOf)
        target = unary->getSubExpr()->IgnoreParenCasts();
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(target);
    const auto *callee =
        ref ? llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()) : nullptr;
    if (!callee)
      return emitError(elementLoc)
             << "unsupported: pointer struct member initializer";
    FailureOr<std::string> name =
        resolveFunctionPointerDecl(callee, fnPtrType, elementLoc);
    if (failed(name))
      return failure();
    Value some = builder
                     .create<emitrust::ConstantOp>(
                         elementLoc, fnPtrType,
                         emitrust::OpaqueAttr::get(
                             builder.getContext(),
                             (llvm::Twine("Some(") + *name + ")").str()))
                     .getResult();
    builder.create<emitrust::AssignOp>(elementLoc, fieldPlace, some);
    return success();
  }
  if (isDataPointer(field->getType())) {
    // A data-pointer field's binding was recorded by the analysis walk of
    // this declaration; the stored i64 member keeps its default 0 (the
    // degenerate binding carries no runtime information), so a supported
    // initializer emits nothing (CTS-P2).
    if (!instance)
      return emitError(elementLoc)
             << "unsupported: pointer struct member initializer in a "
                "nested aggregate";
    MemberPointerKey key{instance->getCanonicalDecl(), field};
    auto it = memberPtrBindings.find(key);
    if (it == memberPtrBindings.end())
      return emitError(elementLoc)
             << "unsupported: pointer struct member initializer";
    if (!it->second.invalidReason.empty()) {
      InFlightDiagnostic diag =
          emitError(elementLoc) << it->second.invalidReason;
      if (it->second.secondLoc.isValid())
        diag.attachNote(translateLoc(it->second.secondLoc))
            << "conflicting binding here";
      return diag;
    }
    return success();
  }
  if (field->isAnonymousStructOrUnion()) {
    // The anonymous member's fields live inline in the parent place; its
    // nested list (the semantic form always materializes one) recurses
    // onto that same place.
    const auto *nested = llvm::dyn_cast<clang::InitListExpr>(element);
    if (!nested)
      return emitError(elementLoc)
             << "unsupported: aggregate initializer element";
    return emitRecordInitFields(
        place, field->getType()->getAsRecordDecl()->getDefinition(), nested,
        instance);
  }
  // A union pun arm stores into its slot's field: the member place takes
  // the SLOT's type and name, and the arm-typed initializer value
  // reinterprets bit-exactly onto it (the local-init counterpart of
  // `reinterpretUnionArmWrite`).
  const clang::FieldDecl *storage = flattenedFieldStorage(field);
  FailureOr<Type> fieldType = mapType(storage->getType(), elementLoc);
  if (failed(fieldType))
    return failure();
  Value fieldPlace = builder
                         .create<emitrust::MemberOp>(
                             elementLoc, emitrust::LValueType::get(*fieldType),
                             place,
                             builder.getStringAttr(flattenedFieldName(field)))
                         .getResult();
  if (storage == field)
    return emitInitListElement(fieldPlace, *fieldType, element);
  FailureOr<Value> value = emitRValue(element);
  if (failed(value))
    return failure();
  return storeToPlace(elementLoc, fieldPlace,
                      reinterpretScalarBits(elementLoc, *value, *fieldType));
}

LogicalResult CImporter::emitInitListElement(Value place, Type type,
                                             const clang::Expr *element) {
  if (const auto *nested = llvm::dyn_cast<clang::InitListExpr>(element))
    return emitAggregateInitList(place, type, nested);
  // A compound-literal element (`{(struct S){1, 2}, ...}`, C99-13) copies
  // a temp that is immediately dead; its list initializes the element
  // place directly, like a nested brace list.
  if (llvm::isa<emitrust::ArrayType, emitrust::StructType>(type))
    if (const auto *compound = llvm::dyn_cast<clang::CompoundLiteralExpr>(
            element->IgnoreParenImpCasts()))
      if (const auto *list = llvm::dyn_cast<clang::InitListExpr>(
              compound->getInitializer()->IgnoreParenImpCasts()))
        return emitAggregateInitList(place, type, list);
  Location loc = translateLoc(element->getBeginLoc());
  // A non-list initializer for an aggregate element (a string literal for
  // a char-array field, a whole-struct copy) is out of scope.
  if (llvm::isa<emitrust::ArrayType, emitrust::StructType>(type))
    return emitError(loc) << "unsupported: aggregate initializer element";
  FailureOr<Value> value = emitRValue(element);
  if (failed(value))
    return failure();
  return storeToPlace(loc, place, *value);
}

LogicalResult
CImporter::emitStringArrayInit(Value place, Type type,
                               const clang::StringLiteral *literal) {
  Location loc = translateLoc(literal->getBeginLoc());
  auto arrayType = llvm::dyn_cast<emitrust::ArrayType>(type);
  // An ordinary literal fills a byte array — signless i8 for plain/signed
  // char elements, unsigned ui8 for unsigned char elements (C99 6.7.8p14
  // admits all three; the bytes are identical since the ASCII policy
  // below keeps every value in 0..=127). A wide literal fills a `wchar_t`
  // (i32 on the supported targets) array, one code unit per element.
  // u8/u/U literals have no mapped element representation.
  if (!literal->isOrdinary() && !literal->isWide())
    return emitError(loc) << "unsupported: non-ordinary string literal "
                             "initializer";
  auto elementType =
      arrayType ? llvm::dyn_cast<IntegerType>(arrayType.getElementType())
                : IntegerType();
  bool elementMatches =
      elementType &&
      (literal->isOrdinary()
           ? elementType.getWidth() == 8 && !elementType.isSigned()
           : elementType.getWidth() == 32 && elementType.isSignless());
  if (!elementMatches)
    return emitError(loc)
           << "unsupported: string literal initializer for this type";
  // C99 6.7.8p14: successive code units of the literal (including the
  // terminating NUL if there is room) initialize the elements; Sema
  // guarantees the literal fits. Elements past the literal keep the
  // place's default zero value (matching C's zero fill), so only the
  // literal's code units plus the NUL are assigned.
  uint64_t length = literal->getLength();
  uint64_t count = std::min<uint64_t>(length + 1, arrayType.getSize());
  for (uint64_t i = 0; i != count; ++i) {
    uint32_t byte = i < length ? literal->getCodeUnit(i) : 0;
    // Non-ASCII bytes of an ordinary literal are rejected so the array's
    // contents stay exact through the ASCII-only `%s`/`%c` printing
    // helpers; a wide array never feeds those helpers.
    if (literal->isOrdinary() && byte > 127)
      return emitError(loc)
             << "unsupported: non-ASCII byte in string literal initializer";
    Value index =
        createIntConstant(loc, builder.getIntegerType(64),
                          static_cast<int64_t>(i));
    Value elementPlace =
        builder
            .create<emitrust::SubscriptOp>(
                loc, emitrust::LValueType::get(arrayType.getElementType()),
                place, index)
            .getResult();
    // createScalarIntConstant covers both the signless (arith) and
    // unsigned (emitrust.constant) element domains.
    Value value = createScalarIntConstant(loc, arrayType.getElementType(),
                                          static_cast<int64_t>(byte));
    if (failed(storeToPlace(loc, elementPlace, value)))
      return failure();
  }
  return success();
}

FailureOr<Value> CImporter::emitCompoundLiteralPlace(
    const clang::CompoundLiteralExpr *literal, bool hoistForRegion) {
  Location loc = translateLoc(literal->getBeginLoc());
  // A file-scope compound literal in a global initializer imports through
  // the constant-evaluator paths (CTS-P4); one reaching an expression
  // context here is defensive.
  if (literal->isFileScope())
    return emitError(loc)
           << "unsupported: file-scope compound literal in expression "
              "position";
  FailureOr<Type> type = mapType(literal->getType(), loc);
  if (failed(type))
    return failure();
  // The C99-13 subset covers aggregate (struct/union/array) literals; a
  // scalar compound literal has no aggregate-init lowering here.
  if (!llvm::isa<emitrust::StructType, emitrust::ArrayType>(*type))
    return emitError(loc)
           << "unsupported: compound literal of non-aggregate type";
  Value place;
  if (hoistForRegion) {
    // A region-base temp is dereferenced wherever the region's pointers
    // are used, so its place must dominate the whole body: create it in
    // the entry block and capture the pristine default value there, then
    // restore that default at each evaluation of the literal so re-runs
    // (a binding inside a loop) re-zero the holes exactly like C's fresh
    // object per evaluation.
    Value defaultValue;
    {
      OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPointToStart(entryBlock);
      place = builder
                  .create<emitrust::VariableOp>(
                      loc, emitrust::LValueType::get(*type))
                  .getResult();
      defaultValue =
          builder.create<emitrust::LoadOp>(loc, *type, place).getResult();
    }
    if (failed(storeToPlace(loc, place, defaultValue)))
      return failure();
  } else {
    place = createVariablePlace(loc, *type);
  }
  const clang::Expr *init = literal->getInitializer()->IgnoreParenImpCasts();
  if (const auto *string = llvm::dyn_cast<clang::StringLiteral>(init)) {
    if (failed(emitStringArrayInit(place, *type, string)))
      return failure();
    return place;
  }
  const auto *list = llvm::dyn_cast<clang::InitListExpr>(init);
  if (!list)
    return emitError(loc) << "unsupported: aggregate initializer";
  // `(char[]){"hi"}`: the braces hold the string as the list's sole
  // element (there is no bare-string spelling for a compound literal);
  // it fills the array like a `char s[] = "..."` declaration.
  if (const clang::InitListExpr *semantic = list->getSemanticForm())
    list = semantic;
  if (llvm::isa<emitrust::ArrayType>(*type) && list->getNumInits() == 1)
    if (const auto *string = llvm::dyn_cast<clang::StringLiteral>(
            list->getInit(0)->IgnoreParenImpCasts())) {
      if (failed(emitStringArrayInit(place, *type, string)))
        return failure();
      return place;
    }
  if (failed(emitAggregateInitList(place, *type, list)))
    return failure();
  return place;
}

FailureOr<const clang::VarDecl *> CImporter::materializeCompoundLiteralBase(
    const clang::CompoundLiteralExpr *literal) {
  const clang::VarDecl *backing =
      literalTemps.getOrCreate(astContext(), literal);
  FailureOr<Value> place =
      emitCompoundLiteralPlace(literal, /*hoistForRegion=*/true);
  if (failed(place))
    return failure();
  // Re-executing the binding (a loop around it) rebinds the backing to the
  // freshly initialized place of that evaluation, matching C's per-block
  // storage duration.
  symbols[backing] = *place;
  return backing;
}

FailureOr<Value>
CImporter::createLiteralBacking(const clang::StringLiteral *literal,
                                Location loc, bool isConst) {
  if (!literal->isOrdinary())
    return emitError(loc) << "unsupported: non-ordinary string literal "
                             "bound to a pointer";
  // The backing holds the literal's bytes plus the terminating NUL, so a
  // strlen-style walk terminates inside the array. Non-ASCII bytes are
  // rejected so the region's contents stay exact through the ASCII-only
  // `%s`/`%c` printing helpers (the emitStringArrayInit policy, C99-28).
  uint64_t length = literal->getLength();
  Type byteType = builder.getIntegerType(8);
  SmallVector<Attribute> bytes;
  bytes.reserve(length + 1);
  for (uint64_t i = 0; i != length; ++i) {
    uint32_t byte = literal->getCodeUnit(i);
    if (byte > 127)
      return emitError(loc) << "unsupported: non-ASCII byte in string "
                               "literal bound to a pointer";
    bytes.push_back(
        IntegerAttr::get(byteType, static_cast<int64_t>(byte)));
  }
  bytes.push_back(IntegerAttr::get(byteType, 0));
  auto arrayType = emitrust::ArrayType::get(builder.getContext(), length + 1,
                                            byteType);
  // Like createVariablePlace, hoist a cached (const) backing to the entry
  // block when the function contains labels so a goto jumping over the
  // declaration cannot leave a later use undominated. A mutable per-call
  // copy is used immediately in the same statement group, so it stays at
  // the current insertion point.
  OpBuilder::InsertionGuard guard(builder);
  if (isConst && currentHasLabels)
    builder.setInsertionPointToStart(entryBlock);
  return builder
      .create<emitrust::VariableOp>(loc, emitrust::LValueType::get(arrayType),
                                    builder.getArrayAttr(bytes), isConst)
      .getResult();
}

FailureOr<Value>
CImporter::getOrCreateLiteralBacking(const clang::StringLiteral *literal,
                                     Location loc) {
  if (Value existing = literalBackings.lookup(literal))
    return existing;
  FailureOr<Value> backing =
      createLiteralBacking(literal, loc, /*isConst=*/true);
  if (failed(backing))
    return failure();
  literalBackings[literal] = *backing;
  return *backing;
}

const clang::StringLiteral *
CImporter::matchStringViewLiteralInit(const clang::VarDecl *var) {
  // W2.12: the one recognized shape is copy-initialization from an
  // ordinary string literal through string_view's `const char*`
  // converting constructor — under C++17's guaranteed elision the AST is
  // exactly ImplicitCastExpr<ConstructorConversion> -> CXXConstructExpr
  // 'void (const char *)' -> ArrayToPointerDecay -> StringLiteral, with
  // any trailing CXXDefaultArgExpr-filled arguments treated as absent
  // (mirroring the std::string literal-ctor classification).
  const clang::Expr *init = var->getInit();
  if (!init)
    return nullptr;
  const clang::Expr *e = init->IgnoreParens();
  if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(e))
    e = cleanups->getSubExpr()->IgnoreParens();
  const auto *conversion = llvm::dyn_cast<clang::ImplicitCastExpr>(e);
  if (!conversion ||
      conversion->getCastKind() != clang::CK_ConstructorConversion)
    return nullptr;
  const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(
      conversion->getSubExpr()->IgnoreParens());
  if (!construct)
    return nullptr;
  const clang::Expr *pointerArg = nullptr;
  for (const clang::Expr *arg : construct->arguments()) {
    if (llvm::isa<clang::CXXDefaultArgExpr>(arg))
      continue;
    if (pointerArg)
      return nullptr; // Two real arguments: the (pointer, count) ctor.
    pointerArg = arg;
  }
  if (!pointerArg)
    return nullptr;
  const auto *decay =
      llvm::dyn_cast<clang::ImplicitCastExpr>(pointerArg->IgnoreParens());
  if (!decay || decay->getCastKind() != clang::CK_ArrayToPointerDecay)
    return nullptr;
  const auto *literal = llvm::dyn_cast<clang::StringLiteral>(
      decay->getSubExpr()->IgnoreParens());
  if (!literal || !literal->isOrdinary())
    return nullptr;
  return literal;
}

LogicalResult
CImporter::emitStringViewLocal(const clang::VarDecl *var,
                               const clang::StringLiteral *literal,
                               Location loc) {
  // W2.12: the decomposition — the literal's shared read-only backing
  // (bytes plus the terminating NUL, reused across every consumer of the
  // same literal in this function), an i64 cursor cell starting at byte
  // 0, and an i64 len cell starting at the literal's length WITHOUT the
  // NUL (a view never includes the terminator). The cells are ordinary
  // entry allocas, so the pipeline's mem2reg/promotion applies unchanged.
  FailureOr<Value> backing = getOrCreateLiteralBacking(literal, loc);
  if (failed(backing))
    return failure();
  IntegerType i64Type = builder.getIntegerType(64);
  Value cursorCell = createEntryAlloca(loc, i64Type);
  Value lenCell = createEntryAlloca(loc, i64Type);
  builder.create<memref::StoreOp>(loc, createIntConstant(loc, i64Type, 0),
                                  cursorCell);
  builder.create<memref::StoreOp>(
      loc,
      createIntConstant(loc, i64Type,
                        static_cast<int64_t>(literal->getLength())),
      lenCell);
  stringViewLocals[var] = StringViewLocalInfo{*backing, cursorCell, lenCell};
  return success();
}

LogicalResult CImporter::emitStringFillLocal(const clang::VarDecl *var,
                                             Location loc) {
  // FR-64: the fused lowering of the C `char *a = malloc(N+1); for (i=0;
  // i<N; ++i) a[i]=C; a[N]='\0';` constant-fill idiom into a single owned
  // `String` binding. The fill loop and NUL store are elided (see
  // `stringFillElidedStmts`); `free(a)` is a no-op; every consumer prints the
  // `String` by `Display`. `planStringFill` proved every soundness clause
  // (constant ASCII fill, unsigned/non-negative count, size >= count+1, no
  // byte read/escape/return), so emission is unconditional here.
  const StringFillFacts &facts = stringFillLocals[var];
  auto stringType = emitrust::OpaqueType::get(builder.getContext(), "String");
  Value place = createVariablePlace(
      loc, stringType,
      var->getName().empty() ? std::string() : mangleMemberName(var->getName()));
  symbols[var] = place;
  // The fill count `N` is imported once at the decl site; it is loop-invariant
  // and reads only variables unwritten in the function, so its value equals
  // the loop's per-iteration bound. Widen to i64 for the `.repeat` count.
  FailureOr<Value> count = emitRValue(facts.countExpr);
  if (failed(count))
    return failure();
  Value count64 = castToIntType(loc, *count, builder.getIntegerType(64));
  Value repeated =
      builder
          .create<emitrust::StringRepeatOp>(
              loc, stringType, count64,
              builder.getStringAttr(std::string(1, facts.fillChar)))
          .getResult();
  return storeToPlace(loc, place, repeated);
}

LogicalResult CImporter::emitVecLocal(const clang::VarDecl *var, Location loc) {
  // FR-65: the lowering of a C `T *a = malloc(n * sizeof(T))` / `calloc(n,
  // sizeof(T))` runtime-sized heap buffer of a non-char scalar element type
  // into a single owned `Vec<T>` binding. The `a[i]` reads/writes become
  // `Vec` index places (`emitSubscriptLValue` routing), and `free(a)` is a
  // no-op — the `Vec` drops at scope end. `planVecLift` proved every soundness
  // clause (non-negative extractable count, index-only + free usage, no
  // escape/alias), so emission is unconditional here.
  const VecFacts &facts = vecValueLocals[var];
  // Spell `Vec<T>` from the mapped element type; the spelling round-trips
  // through `parseStlElementType` at the subscript sites.
  std::optional<std::string> spelling =
      rustSpellingForElementType(facts.elementType);
  if (!spelling)
    return emitError(loc) << "unsupported: Vec element type";
  auto vecType =
      emitrust::OpaqueType::get(builder.getContext(), "Vec<" + *spelling + ">");
  Value place = createVariablePlace(
      loc, vecType,
      var->getName().empty() ? std::string() : mangleMemberName(var->getName()));
  symbols[var] = place;
  // The element count is imported once at the decl site — exactly where
  // `malloc` evaluates it — and widened to i64 for `vec![_; n as usize]`.
  FailureOr<Value> count = emitRValue(facts.countExpr);
  if (failed(count))
    return failure();
  Value count64 = castToIntType(loc, *count, builder.getIntegerType(64));
  // The suffixed zero fill: `0i32`/`0u32`/... for integers, `0.0f32`/`0.0f64`
  // for floats. Zero is the sound refinement of `malloc`'s indeterminate bytes
  // (a defined program writes each element before reading it) and matches
  // `calloc`'s pre-zeroing exactly.
  std::string fill;
  if (llvm::isa<Float32Type>(facts.elementType))
    fill = "0.0f32";
  else if (llvm::isa<Float64Type>(facts.elementType))
    fill = "0.0f64";
  else
    fill = "0" + *spelling;
  Value filled = builder
                     .create<emitrust::VecFillOp>(loc, vecType, count64,
                                                  builder.getStringAttr(fill))
                     .getResult();
  return storeToPlace(loc, place, filled);
}

LogicalResult CImporter::emitFamLocal(const clang::VarDecl *var,
                                      Location loc) {
  // FR-94/95: the owned lowering of a FAM-record heap local. The C
  // `S *d = malloc(sizeof(S) + n)` binds `let mut d: S = S { tail:
  // vec![<zero>; n], ..S::default() }` — the struct variable's tail-member
  // assign fuses into the struct-literal init at translate — and the
  // owned-return call form binds the callee's by-value result. `planFamLift`
  // proved every soundness clause (admitted gap-free record, non-negative
  // side-effect-free count — the BYTE extent for a u8 tail, the extracted
  // ELEMENT count for a typed one — elided null guards), so emission is
  // unconditional here.
  const FamAllocFacts &facts = famAllocLocals[var];
  clang::QualType pointee =
      var->getType().getCanonicalType()->getPointeeType();
  FailureOr<Type> structType = mapType(pointee, loc);
  if (failed(structType))
    return failure();
  if (!llvm::isa<emitrust::StructType>(*structType))
    return emitError(loc) << "unsupported: flexible-array record type";
  Value place = createVariablePlace(
      loc, *structType,
      var->getName().empty() ? std::string()
                             : mangleMemberName(var->getName()));
  symbols[var] = place;
  if (facts.initCall) {
    // `S *d = alloc_fn(...)`: the recognized allocator returns the record
    // BY VALUE; the binding is a plain move into the owned local.
    FailureOr<Value> value = emitCall(facts.initCall);
    if (failed(value))
      return failure();
    if (!*value || (*value).getType() != *structType)
      return emitError(loc)
             << "unsupported: flexible-array record initializer";
    builder.create<emitrust::AssignOp>(loc, place, *value);
    return success();
  }
  return emitFamTailFill(place, pointee->getAsRecordDecl(), facts.countExpr,
                         loc);
}

LogicalResult CImporter::emitFamTailFill(Value place,
                                         const clang::RecordDecl *record,
                                         const clang::Expr *countExpr,
                                         Location loc) {
  const clang::FieldDecl *tail = famTailField(record);
  if (!tail) // Defensive; planFamLift only claims admitted records.
    return emitError(loc) << "unsupported: flexible-array record type";
  auto vecType = famTailVecType(tail);
  Value tailPlace =
      builder
          .create<emitrust::MemberOp>(
              loc, emitrust::LValueType::get(vecType), place,
              builder.getStringAttr(flattenedFieldName(tail)))
          .getResult();
  // The tail count is imported once at the binding site — exactly where
  // malloc evaluates it — and widened to i64 for `vec![<zero>; n as usize]`.
  // The suffixed zero fill (`0u8` for the byte tail, `0i16`/`0.0f32`/... for
  // typed tails, mirroring `emitVecLocal`) is the sound refinement of
  // malloc's indeterminate tail bytes (a defined program writes each
  // element before reading it).
  FailureOr<Value> count = emitRValue(countExpr);
  if (failed(count))
    return failure();
  Value count64 = castToIntType(loc, *count, builder.getIntegerType(64));
  clang::QualType tailElement =
      astContext().getAsArrayType(tail->getType())->getElementType();
  std::string fill = "0u8";
  if (!isU8ScalarType(tailElement)) {
    Type elementType = famTailVecElementType(tailElement);
    if (llvm::isa<Float32Type>(elementType))
      fill = "0.0f32";
    else if (llvm::isa<Float64Type>(elementType))
      fill = "0.0f64";
    else
      fill = "0" + *rustSpellingForElementType(elementType);
  }
  Value filled = builder
                     .create<emitrust::VecFillOp>(loc, vecType, count64,
                                                  builder.getStringAttr(fill))
                     .getResult();
  builder.create<emitrust::AssignOp>(loc, tailPlace, filled);
  return success();
}

LogicalResult CImporter::emitFamOptionMemberAssign(
    const clang::MemberExpr *member, const clang::FieldDecl *field,
    const clang::Expr *rhs, Location loc) {
  // FR-96: `base->field = rhs` on a lifted member-held FAM field.
  FailureOr<Value> optionPlace =
      emitFamOptionMemberPlace(member, field, loc, /*writeback=*/nullptr);
  if (failed(optionPlace))
    return failure();
  FailureOr<emitrust::OpaqueType> optionType = famOptionMemberType(field, loc);
  if (failed(optionType))
    return failure();
  // `base->field = NULL` (and `free(base->field)`, routed here by the free
  // handler as a None store): dropping the old payload IS the deallocation.
  if (isNullPointerConstantExpr(rhs)) {
    Value none = builder
                     .create<emitrust::LiteralOp>(
                         loc, *optionType, builder.getStringAttr("None"))
                     .getResult();
    builder.create<emitrust::AssignOp>(loc, *optionPlace, none);
    return success();
  }
  // The recognized alloc form: a temp record value whose tail binds
  // `vec![<zero>; n]` (the translator fuses the member assign into the
  // struct-literal init — `S { tail: vec![...], ..S::default() }`), moved
  // through `Some(...)` into the member place.
  auto alloc = famMemberAllocAssigns.find(rhs);
  if (alloc == famMemberAllocAssigns.end())
    // Defensive; an unrecognized write poisoned the field in Pass A, so it
    // cannot reach the lifted arm.
    return emitError(loc)
           << "unsupported: pointer struct member assigned this value";
  clang::QualType pointee =
      field->getType().getCanonicalType()->getPointeeType();
  FailureOr<Type> structType = mapType(pointee, loc);
  if (failed(structType))
    return failure();
  if (!llvm::isa<emitrust::StructType>(*structType))
    return emitError(loc) << "unsupported: flexible-array record type";
  Value temp = createVariablePlace(loc, *structType);
  if (failed(emitFamTailFill(temp, pointee->getAsRecordDecl(), alloc->second,
                             loc)))
    return failure();
  Value moved =
      builder.create<emitrust::LoadOp>(loc, *structType, temp).getResult();
  Value some = builder
                   .create<emitrust::CallOpaqueOp>(
                       loc, TypeRange{Type(*optionType)},
                       builder.getStringAttr("Some"),
                       /*args=*/ArrayAttr(), ValueRange{moved})
                   .getResult(0);
  builder.create<emitrust::AssignOp>(loc, *optionPlace, some);
  return success();
}

LogicalResult CImporter::emitPointerLocal(const clang::VarDecl *var,
                                          Location loc) {
  clang::QualType pointee =
      var->getType().getCanonicalType()->getPointeeType();
  if (pointee.getCanonicalType()->isPointerType())
    return emitPointerPointerLocal(var, loc);

  // W4.2e Part B (FR-39): a node-pool handle is a nullable pool index -- an
  // i64 index cell plus an i1 non-null cell (the CTS-P8 shape) -- decomposed
  // against the shared pool as its backing. `h->field` subscripts the pool
  // at the index and projects the member; a null-check reads the flag. The
  // handle bypasses the region-driven model entirely (its pointer never
  // addresses a single object; it selects a pool slot).
  if (poolHandleVars.contains(var)) {
    Value idxCell = createEntryAlloca(loc, builder.getIntegerType(64));
    Value nonNullCell = createEntryAlloca(loc, builder.getI1Type());
    PointerLocalInfo info;
    info.backing = currentPoolPlace;
    info.cursorCell = idxCell;
    info.nonNullCell = nonNullCell;
    pointerLocals[var] = info;
    // Default to None until bound (a handle read before binding is null).
    builder.create<memref::StoreOp>(loc, createBoolConstant(loc, false),
                                    nonNullCell);
    builder.create<memref::StoreOp>(
        loc, createIntConstant(loc, builder.getIntegerType(64), 0), idxCell);
    if (const clang::Expr *init = var->getInit())
      return storePointerAssign(loc, var, init);
    return success();
  }

  const PointerRegion *region = pointerRegions.regionOf(var);
  if (!region)
    return success(); // Declared but never used as a pointer; no code.
  if (!region->invalidReason.empty())
    return emitError(translateLoc(region->invalidLoc))
           << region->invalidReason;
  if (region->literalBase) {
    // Read-only string-literal region: the pointer is a cursor into the
    // literal's `'static` byte run, backed by an immutable local byte
    // array (bytes plus the terminating NUL). Nullable literal regions
    // are outside the CTS-P8 scope.
    if (region->nullable)
      return emitError(translateLoc(region->nullableLoc))
             << "unsupported: null pointer constant assigned to a pointer "
                "into a string literal";
    if (!region->bases.empty()) {
      const PointerBaseBinding &object = region->bases.front();
      InFlightDiagnostic diag = emitError(loc);
      diag << "unsupported: pointer '" << var->getName()
           << "' would join a string literal and object '"
           << object.base->getName() << "' into one region";
      diag.attachNote(translateLoc(region->literalLoc))
          << "bound to a string literal here";
      diag.attachNote(translateLoc(object.loc))
          << "bound to '" << object.base->getName() << "' here";
      return diag;
    }
    if (region->hasWriteThrough)
      return emitError(translateLoc(region->writeThroughLoc))
             << "unsupported: write through a pointer to a string literal "
                "(the literal is read-only)";
    Location bindLoc = translateLoc(region->literalLoc);
    FailureOr<Type> elementType = mapType(pointee, bindLoc);
    if (failed(elementType))
      return failure();
    if (*elementType != builder.getIntegerType(8))
      return emitError(bindLoc)
             << "unsupported: pointer element type does not match its "
                "string literal";
    FailureOr<Value> backing =
        getOrCreateLiteralBacking(region->literalBase, bindLoc);
    if (failed(backing))
      return failure();
    Value cell = createEntryAlloca(loc, builder.getIntegerType(64));
    pointerLocals[var] = PointerLocalInfo{nullptr, cell, *backing};
    if (const clang::Expr *init = var->getInit())
      return storePointerAssign(loc, var, init);
    return success();
  }
  if (region->bases.size() >= 2) {
    // A pointer rebound across distinct objects keeps one region under the
    // enum-of-bases model (CTS-P7): each pointer of the region carries a
    // promotable i32 discriminant cell naming its active base, and every
    // dereference dispatches on it — each variant of the closed enum names
    // a disjoint region, so the disjoint-region invariant is preserved
    // while the objects stay independently addressable. The model requires
    // every base to be local and of one uniform kind — all element runs
    // (arrays or slice parameters) whose element type is the pointee, or
    // all degenerate scalar objects of the pointee type. Anything else
    // (mixed kinds, mismatched element types, global bases, a nullable
    // region) keeps the historical join rejection naming both objects and
    // both binding sites.
    auto joinReject = [&]() -> LogicalResult {
      const PointerBaseBinding &first = region->bases[0];
      const PointerBaseBinding &second = region->bases[1];
      InFlightDiagnostic diag = emitError(loc);
      diag << "unsupported: pointer '" << var->getName()
           << "' would join objects '" << first.base->getName() << "' and '"
           << second.base->getName() << "' into one region";
      diag.attachNote(translateLoc(first.loc))
          << "bound to '" << first.base->getName() << "' here";
      diag.attachNote(translateLoc(second.loc))
          << "bound to '" << second.base->getName() << "' here";
      return diag;
    };
    if (region->nullable)
      return joinReject();
    bool anyCursored = false;
    bool anyDegenerate = false;
    bool anyMember = false;
    // A `void *` pointee is a pointee-wildcard cursor (CTS-P9): it carries
    // no element unit of its own, so the per-base element checks below do
    // not apply; each reinterpret-back deref site type-checks instead.
    bool wildcard = pointee.getCanonicalType()->isVoidType();
    for (const PointerBaseBinding &binding : region->bases) {
      const clang::VarDecl *base = binding.base;
      if (binding.member) {
        // A member base (CTS-P9) is a degenerate one-element run rooted
        // at the member's own place; a global root reuses the staged-copy
        // machinery per dispatch arm.
        if (!wildcard && !astContext().hasSameUnqualifiedType(
                             pointee, binding.member->getType()))
          return joinReject();
        anyDegenerate = true;
        anyMember = true;
        continue;
      }
      if (!base->hasLocalStorage())
        return joinReject();
      if (isPointerType(base->getType())) {
        // A slice-classified pointer parameter base: its deref'd slice
        // place was registered in the prologue with a cursor cell. A
        // byte-region-record pointee (FR-93's window root — the aes cbc
        // shape's `ctx` base next to the walking `buf` parameter) is
        // the flat byte image, walked by a u8 pointee at absolute byte
        // offsets.
        clang::QualType baseElement =
            base->getType().getCanonicalType()->getPointeeType();
        bool elementOk =
            wildcard ||
            astContext().hasSameUnqualifiedType(pointee, baseElement) ||
            (!astContext().getLangOpts().CPlusPlus &&
             isByteRegionAggregate(baseElement) && isU8ScalarType(pointee));
        auto baseInfo = pointerLocals.find(base);
        if (baseInfo == pointerLocals.end() ||
            !baseInfo->second.cursorCell || !elementOk)
          return joinReject();
        anyCursored = true;
      } else if (!astContext().getLangOpts().CPlusPlus &&
                 isByteRegionAggregate(base->getType()) &&
                 isU8ScalarType(pointee)) {
        // FR-93 (C path only): a LOCAL byte-region aggregate base — its
        // own flat byte array, cursored at absolute byte offsets (the
        // dot-root window form of the same shape).
        anyCursored = true;
      } else if (const clang::ConstantArrayType *array =
                     astContext().getAsConstantArrayType(base->getType())) {
        bool matchesLevel = wildcard;
        for (const clang::ConstantArrayType *level = array;
             level && !matchesLevel;
             level = astContext().getAsConstantArrayType(
                 level->getElementType()))
          if (astContext().hasSameUnqualifiedType(pointee,
                                                  level->getElementType()))
            matchesLevel = true;
        if (!matchesLevel)
          return joinReject();
        anyCursored = true;
      } else {
        if (!wildcard &&
            !astContext().hasSameUnqualifiedType(pointee, base->getType()))
          return joinReject();
        anyDegenerate = true;
      }
    }
    if (anyCursored && anyDegenerate)
      return joinReject();
    if (anyDegenerate && region->hasArithmetic)
      return emitError(translateLoc(region->arithmeticLoc))
             << (anyMember ? "unsupported: pointer arithmetic on the "
                             "address of a struct member"
                           : "unsupported: arithmetic on the address of a "
                             "scalar object");
    PointerLocalInfo info;
    if (anyCursored)
      info.cursorCell = createEntryAlloca(loc, builder.getIntegerType(64));
    info.baseIndexCell = createEntryAlloca(loc, builder.getIntegerType(32));
    for (const PointerBaseBinding &binding : region->bases)
      info.multiBases.push_back(PointerBaseKey{binding.base, binding.member});
    pointerLocals[var] = info;
    if (const clang::Expr *init = var->getInit())
      return storePointerAssign(loc, var, init);
    return success();
  }
  if (region->bases.empty()) {
    // A recognized constant-size heap allocation bound to a LOCAL pointer
    // (W4.2e Part A): the pointer decomposes against a synthesized
    // entry-block MUTABLE backing array of `allocCount` elements plus an
    // i64 cursor cell — the writable, function-scope analog of the
    // string-literal region. malloc's indeterminate contents are refined
    // to zero (the backing `emitrust.variable` default-initializes),
    // matching calloc exactly; the cursor starts at 0 (the base of a
    // fresh allocation). Escapes (address-of the pointer, a store into a
    // global, a returned pointer) set `invalidReason`/reject upstream, so
    // a region that reaches here with `allocSite` is a genuine flat buffer.
    if (region->allocSite) {
      FailureOr<Type> elementType = mapType(pointee, loc);
      if (failed(elementType))
        return failure();
      Type backingType = emitrust::ArrayType::get(
          builder.getContext(), region->allocCount, *elementType);
      Value backing = createVariablePlace(loc, backingType);
      Value cursorCell = createEntryAlloca(loc, builder.getIntegerType(64));
      PointerLocalInfo info;
      info.cursorCell = cursorCell;
      info.backing = backing;
      pointerLocals[var] = info;
      // The fresh backing is already zeroed, so the declaration binding
      // only initializes the cursor to 0; a later `p = malloc(...)`
      // re-zeroes the backing through `storePointerAssign`.
      builder.create<memref::StoreOp>(
          loc, createIntConstant(loc, builder.getIntegerType(64), 0),
          cursorCell);
      return success();
    }
    // An integer-carrier region (CTS-P3): the pointer never addresses a
    // modeled object — its only sources are integer-to-pointer casts,
    // carrier-returning calls, and null constants — so its entire runtime
    // state is one plain i64 cell (null is 0). Walking or dereferencing a
    // carrier has nothing to resolve against and rejects here, at the
    // first offending site the analysis recorded.
    if (region->hasCarrierSource) {
      if (region->hasArithmetic)
        return emitError(translateLoc(region->arithmeticLoc))
               << "unsupported: pointer arithmetic on an integer-carrier "
                  "pointer";
      if (region->hasWriteThrough)
        return emitError(translateLoc(region->writeThroughLoc))
               << "unsupported: dereference of an integer-carrier pointer";
      Value cell = createEntryAlloca(loc, builder.getIntegerType(64));
      carrierLocals[var] = cell;
      if (const clang::Expr *init = var->getInit())
        return storePointerAssign(loc, var, init);
      return success();
    }
    // Never bound to any object. A base-less nullable region with a
    // conditional source is STATICALLY NULL (CTS-P9): it only ever unites
    // null constants and other null-only pointers, so it carries zero
    // runtime state — no flag cell is materialized, null tests fold to
    // constants, `(int) p` folds to 0, assignments are no-ops (see
    // `storePointerAssign`), and any dereference is rejected at its site.
    // A base-less nullable region built only from direct null bindings
    // keeps its CTS-P8 Option-of-cursor discriminant so null-checks read
    // the flag. A non-nullable unbound pointer needs no code.
    if (region->nullable && !isStaticallyNullRegion(region)) {
      Value nonNullCell = createEntryAlloca(loc, builder.getI1Type());
      pointerLocals[var] =
          PointerLocalInfo{nullptr, Value(), Value(), nonNullCell};
      if (const clang::Expr *init = var->getInit())
        return storePointerAssign(loc, var, init);
    }
    return success();
  }

  const PointerBaseBinding &binding = region->bases.front();
  // A global (or static-local) base is accepted (CTS-P6): every element
  // access through the pointer stages the global's whole value and writes
  // store the staged copy back, exactly like a direct global element
  // access, so the cursor cell below is the pointer's only runtime state
  // and no borrow of the global is ever held. The base's element/pointee
  // validation reads only the declared type and applies unchanged.
  const clang::VarDecl *base = binding.base;
  Location bindLoc = translateLoc(binding.loc);
  // A `void *` pointee is a pointee-wildcard cursor (CTS-P9): it names no
  // element unit, so the element checks below do not apply; each
  // reinterpret-back deref site (`*(T *)p`) type-checks `T` against the
  // base element type instead.
  bool wildcard = pointee.getCanonicalType()->isVoidType();

  Value cursorCell;
  // FR-93 is C-path-only, and a whole-array member pointer
  // (`int (*p)[4] = &s.arr`, pointee == the member type itself) keeps
  // the degenerate CTS-P9 arm below unchanged.
  const clang::ConstantArrayType *memberArray =
      binding.member && !astContext().getLangOpts().CPlusPlus &&
              !astContext().hasSameUnqualifiedType(
                  pointee, binding.member->getType())
          ? astContext().getAsConstantArrayType(binding.member->getType())
          : nullptr;
  // FR-94/95: an ADMITTED FAM-tail member base (`buf = &d->buffers[k]`,
  // `index = hsi->index`) is a cursored element run over the tail's owned
  // Vec member place; the pointee must be the tail's own element. The void
  // WILDCARD stays u8-only (FR-95): void* cursor arithmetic is
  // byte-granular and would miscompile over a typed Vec, so a typed tail
  // requires the exact element pointee.
  if (binding.member && !memberArray &&
      famTailField(binding.member->getParent()) == binding.member) {
    clang::QualType tailElement =
        astContext()
            .getAsArrayType(binding.member->getType())
            ->getElementType();
    bool tailIsU8 = isU8ScalarType(tailElement);
    bool wildcardTail =
        tailIsU8 && pointee.getCanonicalType()->isVoidType();
    bool matchesElement =
        tailIsU8 ? isU8ScalarType(pointee)
                 : astContext().hasSameUnqualifiedType(pointee, tailElement);
    if (!wildcardTail && !matchesElement)
      return emitError(bindLoc)
             << "unsupported: pointer element type does not match its "
                "target array";
    PointerLocalInfo info;
    info.base = binding.base;
    info.cursorCell = createEntryAlloca(loc, builder.getIntegerType(64));
    info.member = binding.member;
    pointerLocals[var] = info;
    if (const clang::Expr *init = var->getInit())
      return storePointerAssign(loc, var, init);
    return success();
  }
  if (binding.member && memberArray) {
    // FR-93: a member-ARRAY base (`p = s->arr`) is a cursored element
    // run over the member's own place — the (backing, cursor) local
    // convention with a MEMBER-place backing. The pointee must be the
    // member array's element type at some nesting level, mirroring the
    // top-level array branch below; arithmetic walks the cursor inside
    // the member's extent (out-of-extent accesses panic at the
    // subscript, the accepted loud refinement of C's UB).
    bool matchesLevel = wildcard;
    for (const clang::ConstantArrayType *level = memberArray;
         level && !matchesLevel;
         level = astContext().getAsConstantArrayType(
             level->getElementType()))
      if (astContext().hasSameUnqualifiedType(pointee,
                                              level->getElementType()))
        matchesLevel = true;
    if (!matchesLevel)
      return emitError(bindLoc)
             << "unsupported: pointer element type does not match its "
                "target array";
    cursorCell = createEntryAlloca(loc, builder.getIntegerType(64));
  } else if (binding.member) {
    // A `&struct.member` base (CTS-P9) is a degenerate one-element run
    // rooted at the member's own place: no cursor, and any pointer
    // arithmetic would walk past the member into sibling storage, which
    // the member-path binding cannot represent.
    if (region->hasArithmetic)
      return emitError(translateLoc(region->arithmeticLoc))
             << "unsupported: pointer arithmetic on the address of a "
                "struct member";
    if (!wildcard && !astContext().hasSameUnqualifiedType(
                         pointee, binding.member->getType()))
      return emitError(bindLoc)
             << "unsupported: pointer type does not match its target "
                "member";
  } else if (isPointerType(base->getType())) {
    // The base is a slice-classified pointer parameter (the only pointer
    // that can be a region base): the local walks the parameter's element
    // run through its own cursor. The binding registered at the function
    // prologue guarantees the base place is an lvalue<slice>.
    auto baseInfo = pointerLocals.find(base);
    if (baseInfo == pointerLocals.end() || !baseInfo->second.cursorCell)
      return emitError(bindLoc)
             << "unsupported: pointer variable bound to a non-slice "
                "pointer parameter"; // Defensive; classification forbids it.
    clang::QualType baseElement =
        base->getType().getCanonicalType()->getPointeeType();
    // A string-cursor parameter's element run is the pointee of its
    // POINTEE: `const char **s` walks the byte region `*s` points into
    // (CTS 00204).
    if (const auto *baseParam = llvm::dyn_cast<clang::ParmVarDecl>(base);
        baseParam && cursorParams.contains(baseParam))
      baseElement = baseElement.getCanonicalType()->getPointeeType();
    // FR-71: a `void *` parameter admitted as a byte-slice cursor has no
    // pointee of its own; its element unit is the byte element the
    // admission scan proved (the same element the signature's slice
    // carries), so the local's pointee validates against THAT. By
    // construction they agree — the scan required every conversion to
    // name one pointee — so a mismatch here would be a defect, and the
    // located rejection below stays as the defensive net.
    if (const auto *baseParam = llvm::dyn_cast<clang::ParmVarDecl>(base);
        baseParam && baseElement.getCanonicalType()->isVoidType())
      if (const auto *owner = llvm::dyn_cast<clang::FunctionDecl>(
              baseParam->getDeclContext())) {
        clang::QualType admitted = voidByteSliceElem(
            owner, baseParam->getFunctionScopeIndex());
        if (!admitted.isNull())
          baseElement = admitted;
      }
    // FR-93 (C path only): a byte-region-record pointee (the FR-91
    // window root, `uint8_t *Iv = ctx->Iv;`) has no per-element pointee
    // of its own — the parameter's region place IS the flat byte image —
    // so a u8 local cursor walks it at absolute byte offsets.
    if (!astContext().getLangOpts().CPlusPlus &&
        isByteRegionAggregate(baseElement) && isU8ScalarType(pointee)) {
      // Element agreement is byte-for-byte by construction.
    } else if (!wildcard &&
               !astContext().hasSameUnqualifiedType(pointee, baseElement)) {
      return emitError(bindLoc)
             << "unsupported: pointer element type does not match its "
                "target parameter";
    }
    cursorCell = createEntryAlloca(loc, builder.getIntegerType(64));
  } else if (!astContext().getLangOpts().CPlusPlus &&
             isByteRegionAggregate(base->getType()) &&
             isU8ScalarType(pointee)) {
    // FR-93 (C path only): a LOCAL byte-region aggregate base bound
    // through a member window decay (`q = s.iv`): the aggregate is its
    // own flat byte array, walked at absolute byte offsets exactly like
    // a decayed byte array. Non-u8 pointees keep the degenerate
    // whole-object branch below (CTS-BR `struct B *p = &x` unchanged).
    cursorCell = createEntryAlloca(loc, builder.getIntegerType(64));
  } else if (const clang::ConstantArrayType *array =
                 astContext().getAsConstantArrayType(base->getType())) {
    // The pointee must be the element type of the base at some array
    // nesting depth: a row pointer (`char (*)[4]` into `char[2][4]`)
    // matches at the first level, a scalar pointer (`char *`) at the
    // innermost. Either way the cursor counts innermost elements in
    // row-major order.
    bool matchesLevel = wildcard;
    for (const clang::ConstantArrayType *level = array;
         level && !matchesLevel;
         level = astContext().getAsConstantArrayType(
             level->getElementType())) {
      if (astContext().hasSameUnqualifiedType(pointee,
                                              level->getElementType()))
        matchesLevel = true;
    }
    if (!matchesLevel)
      return emitError(bindLoc)
             << "unsupported: pointer element type does not match its "
                "target array";
    cursorCell = createEntryAlloca(loc, builder.getIntegerType(64));
  } else {
    // Degenerate base: the pointer can only ever designate the whole
    // scalar (or struct) object, so it carries no cursor and supports no
    // arithmetic.
    if (region->hasArithmetic)
      return emitError(translateLoc(region->arithmeticLoc))
             << "unsupported: arithmetic on the address of a scalar object";
    if (!wildcard &&
        !astContext().hasSameUnqualifiedType(pointee, base->getType()))
      return emitError(bindLoc)
             << "unsupported: pointer type does not match its target object";
  }
  // A nullable region carries the Option-of-cursor discriminant in a
  // promotable i1 cell per pointer: address bindings store true, null
  // bindings store false, and null-checks load it (CTS-P8).
  Value nonNullCell;
  if (region->nullable)
    nonNullCell = createEntryAlloca(loc, builder.getI1Type());
  PointerLocalInfo info{base, cursorCell, Value(), nonNullCell};
  info.member = binding.member;
  pointerLocals[var] = info;
  if (const clang::Expr *init = var->getInit())
    return storePointerAssign(loc, var, init);
  return success();
}

LogicalResult CImporter::emitPointerPointerLocal(const clang::VarDecl *var,
                                                 Location loc) {
  clang::QualType pointee =
      var->getType().getCanonicalType()->getPointeeType();
  // The selected cell must hold a first-order object pointer: a function
  // pointer is an ordinary Copy value with no cursor cell to select, and a
  // third-order pointer would need a region of second-order selections.
  if (pointee.getCanonicalType()->isFunctionPointerType())
    return emitError(loc)
           << "unsupported: pointer to a function pointer variable";
  if (pointee.getCanonicalType()->getPointeeType()->isPointerType())
    return emitError(loc)
           << "unsupported: pointer-to-pointer-to-pointer variable";

  const SecondOrderRegion *region = pointerRegions.secondOrderRegionOf(var);
  if (!region)
    return success(); // Declared but never used as a pointer; no code.
  if (!region->invalidReason.empty())
    return emitError(translateLoc(region->invalidLoc))
           << region->invalidReason;
  if (region->secondTarget) {
    // A selection over two distinct pointer variables would need a real
    // region of cursor cells with a runtime second-order cursor; the
    // degenerate one-cell shape names both bindings and rejects.
    InFlightDiagnostic diag = emitError(loc);
    diag << "unsupported: pointer-to-pointer '" << var->getName()
         << "' would select between pointer variables '"
         << region->target->getName() << "' and '"
         << region->secondTarget->getName() << "'";
    diag.attachNote(translateLoc(region->targetLoc))
        << "bound to '" << region->target->getName() << "' here";
    diag.attachNote(translateLoc(region->secondTargetLoc))
        << "bound to '" << region->secondTarget->getName() << "' here";
    return diag;
  }
  if (!region->target)
    return success(); // Never bound; any dereference rejects at its site.
  // The degenerate one-cell region: the selection is static, so the
  // binding (and every later `pp = &p` of the same target) emits no code.
  pointerPointerLocals[var] = region->target;
  return success();
}

LogicalResult CImporter::storePointerAssign(Location loc,
                                            const clang::VarDecl *ptr,
                                            const clang::Expr *rhs) {
  // An integer-carrier pointer local (CTS-P3) rebinds by storing the
  // carrier's plain i64 value into its cell; no pointer state exists.
  if (Value cell = carrierLocals.lookup(ptr)) {
    FailureOr<Value> value = emitCarrierValue(rhs);
    if (failed(value))
      return failure();
    builder.create<memref::StoreOp>(loc, *value, cell);
    return success();
  }
  auto it = pointerLocals.find(ptr);
  if (it == pointerLocals.end()) {
    auto globalIt = pointerGlobals.find(ptr->getCanonicalDecl());
    if (globalIt != pointerGlobals.end())
      return storeGlobalPointerAssign(loc, ptr, globalIt->second, rhs);
    auto secondIt = pointerPointerLocals.find(ptr);
    if (secondIt != pointerPointerLocals.end()) {
      // `pp = &p`: the second-order selection is static (the analysis
      // accepted exactly one target), so the rebinding emits no code.
      // Defensively verify the operand is that target's address.
      const auto *unary =
          llvm::dyn_cast<clang::UnaryOperator>(stripTrivia(rhs));
      const clang::VarDecl *target =
          unary && unary->getOpcode() == clang::UO_AddrOf
              ? asLocalVarRef(unary->getSubExpr())
              : nullptr;
      if (target != secondIt->second)
        return emitError(loc) // Defensive; the analysis forbids it.
               << "unsupported: pointer-to-pointer assignment would rebind "
                  "to a different pointer variable";
      return success();
    }
    // A statically-null pointer (a base-less nullable region, CTS-P9)
    // carries zero runtime state: every source the analysis admitted into
    // its region is a null constant or another statically-null pointer,
    // so the assignment is a no-op.
    if (ptr->hasLocalStorage() &&
        isStaticallyNullRegion(pointerRegions.regionOf(ptr)))
      return success();
    return emitError(loc) << "unsupported: assignment to pointer variable '"
                          << ptr->getName() << "' with no known target object";
  }
  // A pointer-typed conditional is a pointer source (CTS-P9): each arm
  // assigns in its own block, so the null/address state merges through the
  // pointer's own flag, discriminant, and cursor cells — no new
  // representation. The implicit `void *` bitcast Sema wraps a mixed-arm
  // conditional in peels first.
  {
    const clang::Expr *peeled = stripTrivia(rhs);
    while (const clang::Expr *sub = peelPointerCast(astContext(), peeled))
      peeled = stripTrivia(sub);
    if (const auto *conditional =
            llvm::dyn_cast<clang::ConditionalOperator>(peeled)) {
      FailureOr<Value> condition = emitCondition(conditional->getCond());
      if (failed(condition))
        return failure();
      Block *trueBlock = createBlock();
      Block *falseBlock = createBlock();
      Block *endBlock = createBlock();
      builder.create<cf::CondBranchOp>(loc, *condition, trueBlock,
                                       ValueRange(), falseBlock, ValueRange());
      builder.setInsertionPointToEnd(trueBlock);
      if (failed(storePointerAssign(loc, ptr, conditional->getTrueExpr())))
        return failure();
      builder.create<cf::BranchOp>(loc, endBlock);
      builder.setInsertionPointToEnd(falseBlock);
      if (failed(storePointerAssign(loc, ptr, conditional->getFalseExpr())))
        return failure();
      builder.create<cf::BranchOp>(loc, endBlock);
      builder.setInsertionPointToEnd(endBlock);
      return success();
    }
  }
  const PointerLocalInfo &info = it->second;
  // W4.2e Part B (FR-39): a node-pool handle assignment (malloc append,
  // NULL, handle copy, or a self-ref field read) has its own lowering.
  if (poolHandleVars.contains(ptr))
    return storePoolHandleAssign(loc, ptr, info, rhs);
  // `p = NULL` selects the None side of the Option-of-cursor model: only
  // the discriminant cell changes (the stale cursor is dead while the
  // flag is false). A pointer without a flag cell cannot represent null;
  // the analysis marks every null-receiving local region nullable, so
  // this rejection covers only non-region pointers (e.g. parameters).
  if (isNullPointerConstantExpr(rhs)) {
    if (!info.nonNullCell)
      return emitError(loc) << "unsupported: null pointer constant assigned "
                               "to this pointer";
    Value none = createBoolConstant(loc, false);
    builder.create<memref::StoreOp>(loc, none, info.nonNullCell);
    return success();
  }
  // `p = malloc(...)` / `p = calloc(...)` on a local backing region (W4.2e
  // Part A): re-zero the synthesized backing and reset the cursor to 0
  // (exact calloc semantics; malloc's indeterminate contents are refined to
  // zero). The region validation admitted exactly one allocation site, so
  // any allocation call reaching this assignment is that site.
  if (info.backing && asAllocCall(rhs)) {
    auto arrayType = llvm::cast<emitrust::ArrayType>(
        llvm::cast<emitrust::LValueType>(info.backing.getType())
            .getValueType());
    Value fresh = createVariablePlace(loc, arrayType);
    Value zeroed =
        builder.create<emitrust::LoadOp>(loc, arrayType, fresh).getResult();
    builder.create<emitrust::AssignOp>(loc, info.backing, zeroed);
    builder.create<memref::StoreOp>(
        loc, createIntConstant(loc, builder.getIntegerType(64), 0),
        info.cursorCell);
    return success();
  }
  // FR-93: a member-array decay source (`p = s->arr;`, `Iv = ctx->Iv;`)
  // resolves through the shared classifier into its member-place (typed)
  // or window (byte-region, absolute byte cursor) decomposition. The
  // interception lives HERE, on the pointer-local binding path, and NOT
  // in `emitPointerRValue`: argument-position member decays must keep
  // their FR-74/86/91 interceptions and historical rejections
  // byte-for-byte (the FAM/const-mut/impure-index/global-root pins).
  FailureOr<PtrExprValue> value = [&]() -> FailureOr<PtrExprValue> {
    const clang::Expr *peeled = stripTrivia(rhs);
    while (const clang::Expr *sub = peelPointerCast(astContext(), peeled))
      peeled = stripTrivia(sub);
    const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(peeled);
    const auto *member =
        decay && decay->getCastKind() == clang::CK_ArrayToPointerDecay
            ? llvm::dyn_cast<clang::MemberExpr>(
                  stripTrivia(decay->getSubExpr()))
            : nullptr;
    if (member)
      if (auto target = classifyMemberArrayDecay(member))
        return emitMemberArrayDecayValue(loc, member, target->first,
                                         target->second);
    // FR-94: `p = &d->tail[k]` over an ADMITTED FAM tail (the heatshrink
    // poll-site window binding) is the member decay at cursor k. Gated on
    // the FAM leaf so constant-extent member arrays keep their pinned
    // frontier ("pointer assigned a non-address value").
    if (const auto *addrOf = llvm::dyn_cast<clang::UnaryOperator>(peeled);
        addrOf && addrOf->getOpcode() == clang::UO_AddrOf)
      if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(
              stripTrivia(addrOf->getSubExpr())))
        if (const auto *subDecay = llvm::dyn_cast<clang::ImplicitCastExpr>(
                stripTrivia(subscript->getBase()));
            subDecay &&
            subDecay->getCastKind() == clang::CK_ArrayToPointerDecay)
          if (const auto *tailMember = llvm::dyn_cast<clang::MemberExpr>(
                  stripTrivia(subDecay->getSubExpr())))
            if (const auto *leaf = llvm::dyn_cast<clang::FieldDecl>(
                    tailMember->getMemberDecl());
                leaf && famTailField(leaf->getParent()) == leaf)
              if (auto target = classifyMemberArrayDecay(tailMember)) {
                FailureOr<Value> index = emitRValue(subscript->getIdx());
                if (failed(index))
                  return failure();
                if (!llvm::isa<IntegerType>((*index).getType()))
                  return emitError(loc)
                         << "unsupported subscript index type";
                Value index64 =
                    castToIntType(loc, *index, builder.getIntegerType(64));
                PtrExprValue value{target->first, index64};
                value.member = target->second;
                return value;
              }
    return emitPointerRValue(rhs);
  }();
  if (failed(value))
    return failure();
  if (!info.multiBases.empty()) {
    // Multi-base region (CTS-P7): the assignment stores the enum-of-bases
    // discriminant alongside the cursor — the bound object's index for an
    // address binding, or the source pointer's own discriminant for
    // `p = q` (every pointer of the region shares the base order).
    Value index;
    if (value->baseIndex) {
      if (value->multiBases != info.multiBases) // Defensive; one region.
        return emitError(loc)
               << "unsupported: pointer assignment would rebind to a "
                  "different object";
      index = value->baseIndex;
    } else {
      const auto *found = llvm::find(
          info.multiBases, PointerBaseKey{value->base, value->member});
      if (!value->base || found == info.multiBases.end()) // Defensive.
        return emitError(loc)
               << "unsupported: pointer assignment would rebind to a "
                  "different object";
      index = createIntConstant(loc, builder.getIntegerType(32),
                                found - info.multiBases.begin());
    }
    builder.create<memref::StoreOp>(loc, index, info.baseIndexCell);
    if (!info.cursorCell)
      return success(); // All-degenerate bases: no element offset to track.
    Value multiCursor =
        value->cursor ? value->cursor
                      : createIntConstant(loc, builder.getIntegerType(64), 0);
    builder.create<memref::StoreOp>(loc, multiCursor, info.cursorCell);
    return success();
  }
  if (value->base != info.base || value->member != info.member ||
      value->literalBacking != info.literalBacking ||
      value->baseIndex) // A multi-base source cannot rebind a single-base
                        // pointer (defensive; regions would have unioned).
    return emitError(loc)
           << "unsupported: pointer assignment would rebind to a different "
              "object";
  // An address binding selects the Some side: the discriminant becomes
  // true (or copies the source pointer's flag on `p = q`).
  if (info.nonNullCell) {
    Value nonNull =
        value->nonNull ? value->nonNull : createBoolConstant(loc, true);
    builder.create<memref::StoreOp>(loc, nonNull, info.nonNullCell);
  }
  if (!info.cursorCell)
    return success(); // Degenerate: the target place is statically known.
  Value cursor = value->cursor
                     ? value->cursor
                     : createIntConstant(loc, builder.getIntegerType(64), 0);
  builder.create<memref::StoreOp>(loc, cursor, info.cursorCell);
  return success();
}

LogicalResult CImporter::emitPairedCursorWrite(const clang::ParmVarDecl *param,
                                               const clang::Expr *rhs,
                                               Location loc) {
  Value place = pairedCursorPlaces.lookup(param);
  if (!place) // Defensive; the prologue binds every planned P parameter.
    return emitError(loc)
           << "unsupported: paired cursor parameter has no bound place";
  FailureOr<PtrExprValue> value = emitPointerRValue(rhs);
  if (failed(value))
    return failure();
  // Planning proved the RHS roots in the mapped co-parameter; verify the
  // decomposition agrees before writing (the co-parameter's slice is the
  // coordinate system the caller's writeback adds its own cursor to).
  const clang::ParmVarDecl *coParam = pairedCursorParams.lookup(param);
  if (value->base != coParam || value->member || value->baseIndex ||
      value->literalBacking)
    return emitError(loc) // Defensive; planning admitted the root.
           << "unsupported: cursor parameter write does not root in a "
              "sibling slice parameter";
  Value cursor = value->cursor
                     ? value->cursor
                     : createIntConstant(loc, builder.getIntegerType(64), 0);
  builder.create<emitrust::AssignOp>(loc, place, cursor);
  return success();
}

LogicalResult CImporter::emitGlobalCursorWrite(const clang::ParmVarDecl *param,
                                               const clang::Expr *rhs,
                                               Location loc) {
  Value place = globalCursorPlaces.lookup(param);
  if (!place) // Defensive; the prologue binds every planned G parameter.
    return emitError(loc)
           << "unsupported: cursor parameter has no bound out-cell place";
  // Re-classify against the C1 grammar so planning and emission can
  // never disagree on what the write means (defensive: planning already
  // admitted exactly this RHS).
  FailureOr<std::optional<GlobalCursorPlan>> shape =
      classifyGlobalCursorWrite(param, rhs, loc);
  if (failed(shape))
    return failure();
  if (!*shape)
    return emitError(loc) // Defensive; planning admitted the RHS.
           << "unsupported: cursor parameter write does not fit the "
              "single-global-or-NULL shape";
  auto assignArm = [&](bool some) {
    Value value =
        builder
            .create<emitrust::LiteralOp>(
                loc, optionCursorType(),
                builder.getStringAttr(some ? "Some(0i64)" : "None"))
            .getResult();
    builder.create<emitrust::AssignOp>(loc, place, value);
  };
  // The whole-global (always-Some) and null (always-None) forms assign
  // directly; the ternary branches and assigns per arm, merging through
  // the referenced cell exactly like `storePointerAssign`'s conditional
  // split.
  if (const auto *conditional =
          llvm::dyn_cast<clang::ConditionalOperator>(stripTrivia(rhs));
      conditional && !isNullPointerConstantExpr(rhs)) {
    bool trueIsSome = !isNullPointerConstantExpr(conditional->getTrueExpr());
    FailureOr<Value> condition = emitCondition(conditional->getCond());
    if (failed(condition))
      return failure();
    Block *trueBlock = createBlock();
    Block *falseBlock = createBlock();
    Block *endBlock = createBlock();
    builder.create<cf::CondBranchOp>(loc, *condition, trueBlock, ValueRange(),
                                     falseBlock, ValueRange());
    builder.setInsertionPointToEnd(trueBlock);
    assignArm(trueIsSome);
    builder.create<cf::BranchOp>(loc, endBlock);
    builder.setInsertionPointToEnd(falseBlock);
    assignArm(!trueIsSome);
    builder.create<cf::BranchOp>(loc, endBlock);
    builder.setInsertionPointToEnd(endBlock);
    return success();
  }
  assignArm(/*some=*/(*shape)->global != nullptr);
  return success();
}

const clang::MemberExpr *
CImporter::asPoolNextFieldRead(const clang::Expr *expr) const {
  const clang::Expr *e = stripTrivia(expr);
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    if (cast->getCastKind() != clang::CK_LValueToRValue &&
        cast->getCastKind() != clang::CK_NoOp)
      break;
    e = stripTrivia(cast->getSubExpr());
  }
  const auto *member = llvm::dyn_cast<clang::MemberExpr>(e);
  if (!member || !member->isArrow())
    return nullptr;
  const auto *field = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
  if (!field || !poolNextFields.contains(field->getCanonicalDecl()))
    return nullptr;
  const clang::VarDecl *base = asLoadedLocalVarRef(member->getBase());
  if (!base)
    base = asLocalVarRef(member->getBase());
  return base && poolHandleVars.contains(base) ? member : nullptr;
}

LogicalResult
CImporter::storePoolHandleAssign(Location loc, const clang::VarDecl *ptr,
                                 const PointerLocalInfo &info,
                                 const clang::Expr *rhs) {
  IntegerType i64Type = builder.getIntegerType(64);
  // `n = malloc(sizeof(struct T))`: append a defaulted slot to the pool and
  // take its index. The handle becomes (index, non-null = true). The
  // high-level `collection_push` hides the free-cursor bump; the pool's
  // slots are default-initialized, so malloc's indeterminate contents are
  // refined to zero.
  if (asAllocCall(rhs)) {
    Value idx = builder
                    .create<emitrust::CollectionPushOp>(loc, i64Type,
                                                        currentPoolPlace)
                    .getResult();
    builder.create<memref::StoreOp>(loc, idx, info.cursorCell);
    builder.create<memref::StoreOp>(loc, createBoolConstant(loc, true),
                                    info.nonNullCell);
    return success();
  }
  // `n = NULL`: the None side; only the flag changes (the index is dead).
  if (isNullPointerConstantExpr(rhs)) {
    builder.create<memref::StoreOp>(loc, createBoolConstant(loc, false),
                                    info.nonNullCell);
    return success();
  }
  // `c = c->next` / `nx = c->next`: destructure the `Option<usize>` field
  // read back into the handle's (non-null, index) pair.
  if (const clang::MemberExpr *fieldRead = asPoolNextFieldRead(rhs)) {
    FailureOr<Value> optValue = emitPoolNextFieldRead(fieldRead, loc);
    if (failed(optValue))
      return failure();
    auto unpack = builder.create<emitrust::CallOpaqueOp>(
        loc, TypeRange{builder.getI1Type(), i64Type},
        builder.getStringAttr("__emitrust_pool_unpack"), ArrayAttr(),
        ValueRange{*optValue});
    builder.create<memref::StoreOp>(loc, unpack.getResult(0), info.nonNullCell);
    builder.create<memref::StoreOp>(loc, unpack.getResult(1), info.cursorCell);
    return success();
  }
  // `head = n`: copy another handle's (index, non-null) pair.
  FailureOr<PtrExprValue> value = emitPointerRValue(rhs);
  if (failed(value))
    return failure();
  if (!value->cursor)
    return emitError(loc)
           << "unsupported: node-pool handle assigned a non-handle value";
  Value nonNull =
      value->nonNull ? value->nonNull : createBoolConstant(loc, true);
  builder.create<memref::StoreOp>(loc, nonNull, info.nonNullCell);
  builder.create<memref::StoreOp>(loc, value->cursor, info.cursorCell);
  return success();
}

FailureOr<Value>
CImporter::emitPoolNextFieldRead(const clang::MemberExpr *member,
                                Location loc) {
  FailureOr<PtrExprValue> handle = emitPointerRValue(member->getBase());
  if (failed(handle))
    return failure();
  FailureOr<Type> pointeeType = mapType(
      member->getBase()->getType().getCanonicalType()->getPointeeType(), loc);
  if (failed(pointeeType))
    return failure();
  FailureOr<Value> basePlace =
      emitPointerPlace(loc, *handle, *pointeeType, /*writeback=*/nullptr);
  if (failed(basePlace))
    return failure();
  const auto *field = llvm::cast<clang::FieldDecl>(member->getMemberDecl());
  Type optType = emitrust::OpaqueType::get(builder.getContext(), "Option<usize>");
  Value fieldPlace =
      builder
          .create<emitrust::MemberOp>(
              loc, emitrust::LValueType::get(optType), *basePlace,
              builder.getStringAttr(flattenedFieldName(field)))
          .getResult();
  return builder.create<emitrust::LoadOp>(loc, optType, fieldPlace).getResult();
}

LogicalResult
CImporter::emitPoolNextFieldAssign(const clang::MemberExpr *member,
                                   const clang::Expr *rhs, Location loc) {
  FailureOr<PtrExprValue> handle = emitPointerRValue(member->getBase());
  if (failed(handle))
    return failure();
  FailureOr<Type> pointeeType = mapType(
      member->getBase()->getType().getCanonicalType()->getPointeeType(), loc);
  if (failed(pointeeType))
    return failure();
  FailureOr<Value> basePlace =
      emitPointerPlace(loc, *handle, *pointeeType, /*writeback=*/nullptr);
  if (failed(basePlace))
    return failure();
  const auto *field = llvm::cast<clang::FieldDecl>(member->getMemberDecl());
  Type optType = emitrust::OpaqueType::get(builder.getContext(), "Option<usize>");
  Value fieldPlace =
      builder
          .create<emitrust::MemberOp>(
              loc, emitrust::LValueType::get(optType), *basePlace,
              builder.getStringAttr(flattenedFieldName(field)))
          .getResult();
  // Build the `Option<usize>` value from the right-hand handle's (non-null,
  // index) pair: NULL is None; another handle is Some(index) when non-null.
  IntegerType i64Type = builder.getIntegerType(64);
  Value some, index;
  if (isNullPointerConstantExpr(rhs)) {
    some = createBoolConstant(loc, false);
    index = createIntConstant(loc, i64Type, 0);
  } else {
    FailureOr<PtrExprValue> value = emitPointerRValue(rhs);
    if (failed(value))
      return failure();
    if (!value->cursor)
      return emitError(loc) << "unsupported: node-pool field assigned a "
                               "non-handle value";
    some = value->nonNull ? value->nonNull : createBoolConstant(loc, true);
    index = value->cursor;
  }
  Value opt = builder
                  .create<emitrust::CallOpaqueOp>(
                      loc, TypeRange{optType},
                      builder.getStringAttr("__emitrust_pool_opt"),
                      ArrayAttr(), ValueRange{some, index})
                  .getResult(0);
  builder.create<emitrust::AssignOp>(loc, fieldPlace, opt);
  return success();
}

LogicalResult
CImporter::storeGlobalPointerAssign(Location loc, const clang::VarDecl *ptr,
                                    const PointerGlobalInfo &info,
                                    const clang::Expr *rhs) {
  IntegerType i64Type = builder.getIntegerType(64);
  // `g = calloc(...)` / `g = malloc(...)`: the region validation accepted
  // exactly one allocation site, so any allocation call reaching an
  // assignment to `g` is that site. Re-zero the synthesized backing by
  // storing a fresh default-initialized value (exact calloc semantics on
  // every execution of the statement; malloc's contents are indeterminate,
  // so zero-filling is a legal refinement) and reset the cursor.
  if (asAllocCall(rhs)) {
    if (info.backingSymbol.empty() || info.cursorSymbol.empty())
      return emitError(loc) // Defensive; the region validation forbids it.
             << "unsupported: allocation assigned to this pointer variable";
    Value fresh = createVariablePlace(loc, info.backingType);
    Value zeroed = builder
                       .create<emitrust::LoadOp>(loc, info.backingType, fresh)
                       .getResult();
    builder.create<emitrust::GlobalStoreOp>(loc, zeroed,
                                            globalSymbol(info.backingSymbol));
    builder.create<emitrust::GlobalStoreOp>(loc,
                                            createIntConstant(loc, i64Type, 0),
                                            globalSymbol(info.cursorSymbol));
    return success();
  }
  FailureOr<PtrExprValue> value = emitPointerRValue(rhs);
  if (failed(value))
    return failure();
  if (value->base != info.base || value->literalBacking ||
      value->baseIndex) // Defensive; global regions are single-base.
    return emitError(loc)
           << "unsupported: pointer assignment would rebind to a different "
              "object";
  if (info.cursorSymbol.empty())
    return success(); // Degenerate: the target place is statically known.
  Value cursor =
      value->cursor ? value->cursor : createIntConstant(loc, i64Type, 0);
  builder.create<emitrust::GlobalStoreOp>(loc, cursor,
                                          globalSymbol(info.cursorSymbol));
  return success();
}

LogicalResult CImporter::emitPointerCompoundAssign(
    const clang::CompoundAssignOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  clang::BinaryOperatorKind opcode =
      clang::BinaryOperator::getOpForCompoundAssignment(op->getOpcode());
  if (opcode != clang::BO_Add && opcode != clang::BO_Sub)
    return emitError(loc) << "unsupported compound assignment on a pointer";
  // Walking a pointer to a whole row would need a row-scaled step (CTS-P
  // scope).
  if (pointsToArray(op->getLHS()->getType()))
    return emitError(loc)
           << "unsupported: arithmetic on a pointer to an array";
  const clang::VarDecl *var = asVarRef(op->getLHS());
  const PointerGlobalInfo *globalInfo = nullptr;
  if (!var)
    if (const clang::VarDecl *global = asGlobalDataPointerRef(op->getLHS())) {
      auto globalIt = pointerGlobals.find(global->getCanonicalDecl());
      if (globalIt != pointerGlobals.end())
        globalInfo = &globalIt->second;
    }
  auto it = var ? pointerLocals.find(var) : pointerLocals.end();
  if (it == pointerLocals.end() && !globalInfo)
    return emitError(loc)
           << "unsupported: compound assignment to this pointer expression";
  if (!globalInfo && !it->second.cursorCell)
    // Defensive; the analysis rejects this at the decl.
    return emitError(loc)
           << "unsupported: arithmetic on the address of a scalar object";
  if (globalInfo && globalInfo->cursorSymbol.empty())
    return emitError(loc)
           << "unsupported: arithmetic on the address of a scalar object";
  IntegerType i64Type = builder.getIntegerType(64);
  Value current =
      globalInfo
          ? builder
                .create<emitrust::GlobalLoadOp>(
                    loc, i64Type, globalSymbol(globalInfo->cursorSymbol))
                .getResult()
          : loadPlace(loc, it->second.cursorCell);
  FailureOr<Value> amount = emitRValue(op->getRHS());
  if (failed(amount))
    return failure();
  auto amountType = llvm::dyn_cast<IntegerType>((*amount).getType());
  if (!amountType)
    return emitError(loc) << "unsupported pointer offset type";
  Value offset = castToIntType(loc, *amount, i64Type);
  Value next =
      opcode == clang::BO_Add
          ? builder.create<arith::AddIOp>(loc, current, offset).getResult()
          : builder.create<arith::SubIOp>(loc, current, offset).getResult();
  if (globalInfo)
    builder.create<emitrust::GlobalStoreOp>(
        loc, next, globalSymbol(globalInfo->cursorSymbol));
  else
    builder.create<memref::StoreOp>(loc, next, it->second.cursorCell);
  return success();
}

LogicalResult CImporter::emitIfStmt(const clang::IfStmt *stmt) {
  Location loc = translateLoc(stmt->getIfLoc());
  // W2.5: C++17 `if (init; cond)` and the condition-declaration form
  // (`if (int x = f())`) desugar by hoisting the declarations into the
  // enclosing block, in source order (the init-statement may itself be
  // followed by a condition declaration). Both are evaluated exactly once
  // in C++, so hoisting is exact; the variable outliving the `if`'s C++
  // scope is unobservable without destructors (outside the subset), and a
  // later same-named declaration is a distinct VarDecl (locals are keyed
  // by decl identity), emitted as an ordinary Rust `let` shadowing. Both
  // getters are always null for C input.
  if (const clang::Stmt *init = stmt->getInit())
    if (failed(emitStmt(init)))
      return failure();
  if (stmt->getConditionVariable())
    if (failed(emitStmt(stmt->getConditionVariableDeclStmt())))
      return failure();
  // A compile-time-constant, side-effect-free condition (a literal, a
  // folded `__builtin_expect(!!(0), 0)`, ...) elides the dead arm BEFORE
  // lowering, so a dead arm may contain constructs that could never lower
  // (an unimportable call, a `_Bool` conversion, a declaration). The
  // elision is gated on a live-label check: a goto-targeted label in the
  // dead arm keeps the arm reachable, and a case/default label of an
  // enclosing switch must not be dropped either — both shapes keep the
  // full lowering below (`containsLabelStmt` scans all descendants;
  // `findNestedSwitchLabel` skips nested switches, whose labels are their
  // own and are elided soundly with them).
  clang::Expr::EvalResult conditionValue;
  if (stmt->getCond()->EvaluateAsInt(conditionValue, astContext())) {
    bool truth = conditionValue.Val.getInt() != 0;
    const clang::Stmt *live = truth ? stmt->getThen() : stmt->getElse();
    const clang::Stmt *dead = truth ? stmt->getElse() : stmt->getThen();
    if (!dead || (!containsLabelStmt(dead) &&
                  !llvm::isa<clang::SwitchCase>(dead) &&
                  !findNestedSwitchLabel(dead))) {
      if (live)
        return emitStmt(live);
      return success();
    }
  }
  FailureOr<Value> condition = emitCondition(stmt->getCond());
  if (failed(condition))
    return failure();

  Block *thenBlock = createBlock();
  Block *elseBlock = stmt->getElse() ? createBlock() : nullptr;
  Block *contBlock = createBlock();
  builder.create<cf::CondBranchOp>(loc, *condition, thenBlock, ValueRange(),
                                   elseBlock ? elseBlock : contBlock,
                                   ValueRange());

  builder.setInsertionPointToEnd(thenBlock);
  if (failed(emitStmt(stmt->getThen())))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, contBlock);

  if (const clang::Stmt *elseStmt = stmt->getElse()) {
    builder.setInsertionPointToEnd(elseBlock);
    if (failed(emitStmt(elseStmt)))
      return failure();
    if (!isTerminated(builder.getInsertionBlock()))
      builder.create<cf::BranchOp>(loc, contBlock);
  }

  builder.setInsertionPointToEnd(contBlock);
  return success();
}

/// Whether any descendant of `stmt` is a DeclRefExpr naming `var`.
/// Conservative body scan for emitCXXForRangeStmt: the original (not
/// desugared) body never references the compiler-internal __range/__begin/
/// __end variables, so any hit is a genuine source-level use of the range.
static bool referencesDecl(const clang::Stmt *stmt,
                           const clang::ValueDecl *var) {
  if (!stmt)
    return false;
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
    if (ref->getDecl() == var)
      return true;
  for (const clang::Stmt *child : stmt->children())
    if (referencesDecl(child, var))
      return true;
  return false;
}

LogicalResult
CImporter::emitCXXForRangeStmt(const clang::CXXForRangeStmt *stmt) {
  Location loc = translateLoc(stmt->getForLoc());
  // R3 (end-evaluation semantics): C++ evaluates the range's end() ONCE;
  // this desugar re-reads len() each iteration. The two agree exactly
  // when the container's LENGTH cannot change during the loop, which the
  // two restrictions below guarantee: the range must be a bare local
  // DeclRefExpr (no temporaries, no calls), and the loop body may not
  // name the range variable at all — so the only access path into the
  // container is the loop variable, and an element write (the `&` form)
  // can never change the length. Everything else keeps a located
  // rejection; the CFG shape mirrors emitForStmt's generic lowering.
  if (stmt->getInit())
    return emitError(loc)
           << "unsupported: ranged-for init-statement";
  const clang::Expr *rangeInit = stmt->getRangeInit()->IgnoreParenImpCasts();
  const auto *rangeRef = llvm::dyn_cast<clang::DeclRefExpr>(rangeInit);
  const auto *rangeVar =
      rangeRef ? llvm::dyn_cast<clang::VarDecl>(rangeRef->getDecl()) : nullptr;
  if (!rangeVar)
    return emitError(loc)
           << "unsupported: ranged-for range must be a named local "
              "container";
  auto symbolIt = symbols.find(rangeVar);
  if (symbolIt == symbols.end())
    return emitError(loc)
           << "unsupported: ranged-for range must be a named local "
              "container";
  Value rangePlace = symbolIt->second;
  auto rangeLValue =
      llvm::dyn_cast<emitrust::LValueType>(rangePlace.getType());
  if (!rangeLValue)
    return emitError(loc)
           << "unsupported: ranged-for range must be a named local "
              "container";
  // Element type: a Vec<T> opaque or a std::array-mapped !emitrust.array.
  auto vecType =
      llvm::dyn_cast<emitrust::OpaqueType>(rangeLValue.getValueType());
  auto arrayType =
      llvm::dyn_cast<emitrust::ArrayType>(rangeLValue.getValueType());
  Type elementType;
  if (vecType && isStlOpaqueType(vecType) &&
      vecType.getValue().starts_with("Vec<")) {
    llvm::StringRef spelling = vecType.getValue();
    elementType = parseStlElementType(spelling.substr(4, spelling.size() - 5));
    if (!elementType)
      return emitError(loc) << "unsupported: ranged-for element type";
  } else if (arrayType) {
    elementType = arrayType.getElementType();
  } else {
    return emitError(loc)
           << "unsupported: ranged-for range is not a recognized container";
  }
  if (referencesDecl(stmt->getBody(), rangeVar))
    return emitError(loc)
           << "unsupported: ranged-for body may not use the range variable";
  const clang::VarDecl *loopVar = stmt->getLoopVariable();
  if (!loopVar)
    return emitError(loc) << "unsupported: ranged-for loop variable shape";
  bool byReference = loopVar->getType()->isReferenceType();
  if (!byReference) {
    FailureOr<Type> mappedVar = mapType(loopVar->getType(), loc);
    if (failed(mappedVar))
      return failure();
    if (*mappedVar != elementType)
      return emitError(loc)
             << "unsupported: ranged-for loop variable type does not match "
                "the element type";
  }

  // i = 0; while (i < len) { <bind loop var to element i>; body; i += 1 }
  // The counter is i64, not index-typed: `convert-to-emitrust` legalizes
  // integer arith.cmpi but not an index-typed one, and an i64 subscript
  // index renders as `[i as usize]`, matching the existing at()/[] paths.
  Type i64 = builder.getIntegerType(64);
  Value indexCell = createEntryAlloca(loc, i64);
  Value zero =
      builder.create<arith::ConstantOp>(loc, builder.getIntegerAttr(i64, 0))
          .getResult();
  builder.create<memref::StoreOp>(loc, zero, indexCell);

  Block *condBlock = createBlock();
  Block *bodyBlock = createBlock();
  Block *incBlock = createBlock();
  Block *exitBlock = createBlock();
  builder.create<cf::BranchOp>(loc, condBlock);

  builder.setInsertionPointToEnd(condBlock);
  Value current = builder.create<memref::LoadOp>(loc, indexCell).getResult();
  Value bound;
  if (vecType) {
    Value len = builder
                    .create<emitrust::MethodCallOp>(
                        loc, TypeRange{builder.getIndexType()}, rangePlace,
                        builder.getStringAttr("len"), ValueRange{})
                    .getResult(0);
    bound = builder.create<emitrust::CastOp>(loc, i64, len).getResult();
  } else {
    bound = builder
                .create<arith::ConstantOp>(
                    loc, builder.getIntegerAttr(
                             i64, static_cast<int64_t>(arrayType.getSize())))
                .getResult();
  }
  Value inBounds =
      builder
          .create<arith::CmpIOp>(loc, arith::CmpIPredicate::slt, current,
                                 bound)
          .getResult();
  builder.create<cf::CondBranchOp>(loc, inBounds, bodyBlock, ValueRange(),
                                   exitBlock, ValueRange());

  builder.setInsertionPointToEnd(bodyBlock);
  Value bodyIndex = builder.create<memref::LoadOp>(loc, indexCell).getResult();
  Value elementPlace =
      builder
          .create<emitrust::SubscriptOp>(
              loc, emitrust::LValueType::get(elementType), rangePlace,
              bodyIndex)
          .getResult();
  if (byReference) {
    // The reference loop variable IS the element place: reads load it,
    // writes assign through it, exactly like a C++ reference local. The
    // subscript re-executes per iteration at runtime, so every use in the
    // body sees the current element.
    symbols[loopVar] = elementPlace;
  } else {
    // The by-value loop variable is a fresh per-iteration copy in its own
    // place (writes to it never touch the container).
    Value element = loadPlace(loc, elementPlace);
    Value varPlace = createVariablePlace(
        loc, elementType,
        loopVar->getName().empty() ? std::string()
                                   : mangleMemberName(loopVar->getName()));
    if (failed(storeToPlace(loc, varPlace, element)))
      return failure();
    symbols[loopVar] = varPlace;
  }
  loopStack.push_back({exitBlock, incBlock});
  LogicalResult bodyResult = emitStmt(stmt->getBody());
  loopStack.pop_back();
  if (failed(bodyResult))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, incBlock);

  builder.setInsertionPointToEnd(incBlock);
  Value beforeInc = builder.create<memref::LoadOp>(loc, indexCell).getResult();
  Value one =
      builder.create<arith::ConstantOp>(loc, builder.getIntegerAttr(i64, 1))
          .getResult();
  Value next = builder.create<arith::AddIOp>(loc, beforeInc, one).getResult();
  builder.create<memref::StoreOp>(loc, next, indexCell);
  builder.create<cf::BranchOp>(loc, condBlock);

  builder.setInsertionPointToEnd(exitBlock);
  return success();
}

LogicalResult CImporter::emitWhileStmt(const clang::WhileStmt *stmt) {
  Location loc = translateLoc(stmt->getWhileLoc());
  if (stmt->getConditionVariable())
    return emitError(loc) << "unsupported: declaration in while condition";

  Block *condBlock = createBlock();
  Block *bodyBlock = createBlock();
  Block *exitBlock = createBlock();
  builder.create<cf::BranchOp>(loc, condBlock);

  builder.setInsertionPointToEnd(condBlock);
  FailureOr<Value> condition = emitCondition(stmt->getCond());
  if (failed(condition))
    return failure();
  builder.create<cf::CondBranchOp>(loc, *condition, bodyBlock, ValueRange(),
                                   exitBlock, ValueRange());

  builder.setInsertionPointToEnd(bodyBlock);
  loopStack.push_back({exitBlock, condBlock});
  LogicalResult bodyResult = emitStmt(stmt->getBody());
  loopStack.pop_back();
  if (failed(bodyResult))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, condBlock);

  builder.setInsertionPointToEnd(exitBlock);
  return success();
}

LogicalResult CImporter::emitForStmt(const clang::ForStmt *stmt) {
  Location loc = translateLoc(stmt->getForLoc());
  if (stmt->getConditionVariable())
    return emitError(loc) << "unsupported: declaration in for condition";
  // FR-61f: a canonical ascending counting loop lifts to `emitrust.for`
  // (`for i in LO..HI`). Every non-canonical shape returns nullopt and falls
  // through to the CFG `while` lowering below — reject-to-the-already-correct
  // lowering is the whole safety story.
  if (std::optional<RangeFor> range = matchRangeFor(stmt))
    return emitRangeFor(*range, stmt);
  if (const clang::Stmt *init = stmt->getInit())
    if (failed(emitStmt(init)))
      return failure();

  Block *condBlock = createBlock();
  Block *bodyBlock = createBlock();
  Block *incBlock = createBlock();
  Block *exitBlock = createBlock();
  builder.create<cf::BranchOp>(loc, condBlock);

  builder.setInsertionPointToEnd(condBlock);
  if (const clang::Expr *cond = stmt->getCond()) {
    FailureOr<Value> condition = emitCondition(cond);
    if (failed(condition))
      return failure();
    builder.create<cf::CondBranchOp>(loc, *condition, bodyBlock, ValueRange(),
                                     exitBlock, ValueRange());
  } else {
    builder.create<cf::BranchOp>(loc, bodyBlock);
  }

  builder.setInsertionPointToEnd(bodyBlock);
  loopStack.push_back({exitBlock, incBlock});
  LogicalResult bodyResult = emitStmt(stmt->getBody());
  loopStack.pop_back();
  if (failed(bodyResult))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, incBlock);

  builder.setInsertionPointToEnd(incBlock);
  if (const clang::Expr *inc = stmt->getInc())
    if (failed(emitExprStmt(inc)))
      return failure();
  builder.create<cf::BranchOp>(loc, condBlock);

  builder.setInsertionPointToEnd(exitBlock);
  return success();
}

//===----------------------------------------------------------------------===//
// FR-61f: range-`for` lift (AST place-emission variant)
//===----------------------------------------------------------------------===//

namespace {

/// Whether `e` (ignoring parens and implicit casts) is a reference to `var`.
bool isRefTo(const clang::Expr *e, const clang::VarDecl *var) {
  const auto *ref =
      llvm::dyn_cast<clang::DeclRefExpr>(e->IgnoreParenImpCasts());
  return ref && ref->getDecl() == var;
}

/// Whether `e` is a positive integer constant; writes its value to `out`.
bool positiveConst(const clang::Expr *e, clang::ASTContext &ctx,
                   int64_t &out) {
  std::optional<llvm::APSInt> value = e->getIntegerConstantExpr(ctx);
  if (!value || value->isNonPositive())
    return false;
  out = value->getExtValue();
  return true;
}

/// Clause 3: matches an ascending unit/constant-step increment on `iv`
/// (`iv++`, `++iv`, `iv += K`, `iv = iv + K`, `iv = K + iv`), writing the
/// positive step to `step`.
bool matchStep(const clang::Expr *inc, const clang::VarDecl *iv,
               clang::ASTContext &ctx, int64_t &step) {
  const clang::Expr *e = inc->IgnoreParenImpCasts();
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e)) {
    if ((unary->getOpcode() == clang::UO_PostInc ||
         unary->getOpcode() == clang::UO_PreInc) &&
        isRefTo(unary->getSubExpr(), iv)) {
      step = 1;
      return true;
    }
    return false;
  }
  // CompoundAssignOperator derives from BinaryOperator, so it must be
  // checked first.
  if (const auto *compound =
          llvm::dyn_cast<clang::CompoundAssignOperator>(e)) {
    if (compound->getOpcode() == clang::BO_AddAssign &&
        isRefTo(compound->getLHS(), iv))
      return positiveConst(compound->getRHS(), ctx, step);
    return false;
  }
  if (const auto *assign = llvm::dyn_cast<clang::BinaryOperator>(e)) {
    if (assign->getOpcode() != clang::BO_Assign ||
        !isRefTo(assign->getLHS(), iv))
      return false;
    const auto *add = llvm::dyn_cast<clang::BinaryOperator>(
        assign->getRHS()->IgnoreParenImpCasts());
    if (!add || add->getOpcode() != clang::BO_Add)
      return false;
    if (isRefTo(add->getLHS(), iv))
      return positiveConst(add->getRHS(), ctx, step);
    if (isRefTo(add->getRHS(), iv))
      return positiveConst(add->getLHS(), ctx, step);
    return false;
  }
  return false;
}

/// Clause 4/5 helper: whether the subtree assigns to, or increments/
/// decrements, `var`.
bool stmtWritesVar(const clang::Stmt *stmt, const clang::VarDecl *var) {
  if (!stmt)
    return false;
  if (const auto *bo = llvm::dyn_cast<clang::BinaryOperator>(stmt))
    if (bo->isAssignmentOp() && isRefTo(bo->getLHS(), var))
      return true;
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt))
    if (unary->isIncrementDecrementOp() && isRefTo(unary->getSubExpr(), var))
      return true;
  for (const clang::Stmt *child : stmt->children())
    if (stmtWritesVar(child, var))
      return true;
  return false;
}

/// Whether the subtree contains a call (used to keep `HI` side-effect free).
bool stmtContainsCall(const clang::Stmt *stmt) {
  if (!stmt)
    return false;
  if (llvm::isa<clang::CallExpr>(stmt))
    return true;
  for (const clang::Stmt *child : stmt->children())
    if (stmtContainsCall(child))
      return true;
  return false;
}

/// Collects the `VarDecl`s a subtree references.
void collectRefVars(const clang::Stmt *stmt,
                    llvm::SmallPtrSetImpl<const clang::VarDecl *> &out) {
  if (!stmt)
    return;
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
      out.insert(var);
  for (const clang::Stmt *child : stmt->children())
    collectRefVars(child, out);
}

/// Whether emitting `stmt` would create cf basic blocks in the function
/// region (nested control flow, short-circuit `&&`/`||`, `?:`, statement
/// expressions) or install a loop exit/jump edge (`break`/`continue`/`goto`/
/// `return`/labels/`case`). An `emitrust.for` body is a single-block region,
/// so any of these in the body makes the loop non-liftable in this slice.
bool blocksRangeForLift(const clang::Stmt *stmt) {
  if (!stmt)
    return false;
  if (llvm::isa<clang::IfStmt, clang::WhileStmt, clang::ForStmt,
                clang::DoStmt, clang::SwitchStmt, clang::CXXForRangeStmt,
                clang::BreakStmt, clang::ContinueStmt, clang::GotoStmt,
                clang::IndirectGotoStmt, clang::ReturnStmt, clang::LabelStmt,
                clang::CaseStmt, clang::DefaultStmt,
                clang::ConditionalOperator, clang::BinaryConditionalOperator,
                clang::StmtExpr>(stmt))
    return true;
  if (const auto *bo = llvm::dyn_cast<clang::BinaryOperator>(stmt))
    if (bo->getOpcode() == clang::BO_LAnd ||
        bo->getOpcode() == clang::BO_LOr)
      return true;
  for (const clang::Stmt *child : stmt->children())
    if (blocksRangeForLift(child))
      return true;
  return false;
}

} // namespace

std::optional<RangeFor>
CImporter::matchRangeFor(const clang::ForStmt *stmt) {
  const clang::Stmt *init = stmt->getInit();
  const clang::Expr *cond = stmt->getCond();
  const clang::Expr *inc = stmt->getInc();
  const clang::Stmt *body = stmt->getBody();
  if (!init || !cond || !inc || !body)
    return std::nullopt;

  // Clause 1 (+6): the init declares the induction `int i = LO;`. Only the
  // DeclStmt form is accepted in v1 -- an init-declared `i` is loop-scoped in
  // C, so it cannot be referenced after the loop and clause 6 (no escape)
  // holds for free. The assignment form `i = LO;` (i declared outside) could
  // escape and falls back to the CFG `while`.
  const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(init);
  if (!declStmt || !declStmt->isSingleDecl())
    return std::nullopt;
  const auto *iv = llvm::dyn_cast<clang::VarDecl>(declStmt->getSingleDecl());
  if (!iv || !iv->isLocalVarDecl())
    return std::nullopt;
  // The induction must be exactly `int` (i32): this guarantees the bounds and
  // step share the ForOp's single operand type (AllTypesMatch) with no width
  // juggling.
  if (iv->getType().getCanonicalType() != astContext().IntTy)
    return std::nullopt;
  const clang::Expr *lo = iv->getInit();
  if (!lo)
    return std::nullopt;

  // Clause 2: cond is `i < HI` (half-open) or `i <= HI` (inclusive, the
  // FR-61f widening slice — renders `..=`; C's only divergence is the
  // final `i++` overflow at HI == INT_MAX, which is C UB, so Rust's
  // cleanly-terminating `..=` is a legal refinement). Descending (`>`,
  // `>=`) still falls back.
  const auto *cmp =
      llvm::dyn_cast<clang::BinaryOperator>(cond->IgnoreParenImpCasts());
  if (!cmp ||
      (cmp->getOpcode() != clang::BO_LT &&
       cmp->getOpcode() != clang::BO_LE) ||
      !isRefTo(cmp->getLHS(), iv))
    return std::nullopt;
  bool inclusive = cmp->getOpcode() == clang::BO_LE;
  const clang::Expr *hi = cmp->getRHS();

  // Clause 3: inc is `i++`/`++i` (K=1) or `i += K`/`i = i + K` with K a
  // positive integer constant.
  int64_t step = 0;
  if (!matchStep(inc, iv, astContext(), step))
    return std::nullopt;

  // Clause 4: `i` is body-immutable -- never written in the body and `&i`
  // never taken anywhere in the function.
  if (addressTaken.contains(iv) || stmtWritesVar(body, iv))
    return std::nullopt;

  // Clause 5: `HI` is loop-invariant and side-effect free. A constant bound
  // is trivially invariant; otherwise every var it reads must be unwritten in
  // the body and it must not read the induction.
  if (hi->HasSideEffects(astContext()) || stmtContainsCall(hi))
    return std::nullopt;
  if (!hi->getIntegerConstantExpr(astContext())) {
    llvm::SmallPtrSet<const clang::VarDecl *, 8> hiVars;
    collectRefVars(hi, hiVars);
    for (const clang::VarDecl *var : hiVars) {
      if (var == iv || stmtWritesVar(body, var))
        return std::nullopt;
    }
  }

  // No exit edge exists in an `emitrust.for`: reject any body that would emit
  // cf blocks or jump.
  if (blocksRangeForLift(body))
    return std::nullopt;

  // The body is a single-block region: nothing it touches may be backed by a
  // `memref.alloca` cell, because mem2reg cannot promote a cell whose
  // load/store lives inside the region op and `convert-to-emitrust` then
  // rejects the leftover alloca (spike 61f-0). The one exception is an
  // automatic-storage integer local, which `collectRangeForPlaceScalars`
  // pre-marks into `placeBackedScalars` so it materializes as an
  // `emitrust.variable` place instead of a cell. Whitelist accordingly:
  //   - the induction (materialized by emitRangeFor itself);
  //   - an automatic integer local (placed);
  //   - an automatic integer-element array (already an `emitrust` place).
  // Everything else -- parameters (memref cells), decomposed pointers (i64
  // cursor cells), globals/statics, floats, structs -- rejects to the CFG
  // `while` lowering. Deliberately narrow: a green suite with partial
  // coverage beats a broad matcher that miscompiles.
  llvm::SmallPtrSet<const clang::VarDecl *, 8> bodyVars;
  collectRefVars(body, bodyVars);
  for (const clang::VarDecl *var : bodyVars) {
    if (var == iv)
      continue;
    if (!var->isLocalVarDecl() || !var->hasLocalStorage())
      return std::nullopt;
    clang::QualType varType = var->getType().getCanonicalType();
    if (varType->isIntegerType())
      continue;
    if (const clang::ArrayType *array = astContext().getAsArrayType(varType))
      if (array->getElementType().getCanonicalType()->isIntegerType())
        continue;
    return std::nullopt;
  }

  return RangeFor{iv, lo, hi, step, inclusive};
}

LogicalResult CImporter::emitRangeFor(const RangeFor &range,
                                      const clang::ForStmt *stmt) {
  Location loc = translateLoc(stmt->getForLoc());
  Type intType = builder.getI32Type();

  // Bounds are evaluated once, in the current block, before the loop.
  FailureOr<Value> lo = emitRValue(range.lo);
  if (failed(lo))
    return failure();
  FailureOr<Value> hi = emitRValue(range.hi);
  if (failed(hi))
    return failure();
  Value loValue = *lo;
  Value hiValue = *hi;
  // The induction is `int`; coerce any differently-typed bound to i32 so the
  // ForOp's three operands share a type (AllTypesMatch).
  if (loValue.getType() != intType)
    loValue = builder.create<emitrust::CastOp>(loc, intType, loValue);
  if (hiValue.getType() != intType)
    hiValue = builder.create<emitrust::CastOp>(loc, intType, hiValue);
  Value stepValue = createIntConstant(loc, intType, range.step);

  auto forOp =
      builder.create<emitrust::ForOp>(loc, loValue, hiValue, stepValue,
                                      /*inclusive=*/range.inclusive);

  // The induction block argument carries the C source name as a `NameLoc`
  // (the FR-61e slice-3 path in the emitter reads it), so it renders
  // `for i in LO..HI` -- snake_case idiomatic / verbatim under
  // `--preserve-c-names` -- or `for _i in ..` when the body never reads it.
  Location ivLoc =
      range.iv->getName().empty()
          ? loc
          : Location(NameLoc::get(
                builder.getStringAttr(mangleMemberName(range.iv->getName())),
                loc));
  Region &region = forOp.getRegion();
  Block *bodyBlock;
  {
    OpBuilder::InsertionGuard guard(builder);
    bodyBlock = builder.createBlock(&region, region.end(), TypeRange{intType},
                                    {ivLoc});
  }

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(bodyBlock);

  // The induction is body-immutable (matcher clause 4) and non-address-taken,
  // so every read is the block-argument value directly -- no place, no seed
  // store, no redundant `let i` binding. `emitRValue`'s scalar-read entry
  // consults `inductionValues` before the ordinary place path. The entry
  // stays registered while a nested loop body emits (a nested body may read
  // this induction) and is cleared per function.
  inductionValues[range.iv] = bodyBlock->getArgument(0);

  if (failed(emitStmt(stmt->getBody())))
    return failure();

  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<emitrust::YieldOp>(loc);
  return success();
}

void CImporter::collectRangeForPlaceScalars(const clang::Stmt *stmt) {
  if (!stmt)
    return;
  if (const auto *forStmt = llvm::dyn_cast<clang::ForStmt>(stmt))
    if (std::optional<RangeFor> range = matchRangeFor(forStmt)) {
      llvm::SmallPtrSet<const clang::VarDecl *, 8> refs;
      collectRefVars(forStmt->getBody(), refs);
      for (const clang::VarDecl *var : refs) {
        // The induction is materialized by `emitRangeFor` itself; params are
        // SSA block args and globals are already places -- only body-local
        // integer scalars would otherwise take the un-promotable
        // `memref.alloca` cell path.
        if (var == range->iv || !var->isLocalVarDecl() ||
            !var->hasLocalStorage() || !var->getType()->isIntegerType())
          continue;
        placeBackedScalars.insert(var);
      }
    }
  for (const clang::Stmt *child : stmt->children())
    collectRangeForPlaceScalars(child);
}

LogicalResult CImporter::emitDoStmt(const clang::DoStmt *stmt) {
  Location loc = translateLoc(stmt->getDoLoc());

  Block *bodyBlock = createBlock();
  Block *condBlock = createBlock();
  Block *exitBlock = createBlock();
  // The body runs at least once: enter it unconditionally.
  builder.create<cf::BranchOp>(loc, bodyBlock);

  builder.setInsertionPointToEnd(bodyBlock);
  loopStack.push_back({exitBlock, condBlock});
  LogicalResult bodyResult = emitStmt(stmt->getBody());
  loopStack.pop_back();
  if (failed(bodyResult))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, condBlock);

  builder.setInsertionPointToEnd(condBlock);
  FailureOr<Value> condition = emitCondition(stmt->getCond());
  if (failed(condition))
    return failure();
  builder.create<cf::CondBranchOp>(loc, *condition, bodyBlock, ValueRange(),
                                   exitBlock, ValueRange());

  builder.setInsertionPointToEnd(exitBlock);
  return success();
}

LogicalResult CImporter::emitSwitchStmt(const clang::SwitchStmt *stmt) {
  Location loc = translateLoc(stmt->getSwitchLoc());
  // W2.5: `switch (init; cond)` and a condition declaration hoist into the
  // enclosing block exactly like emitIfStmt's desugar (evaluated once;
  // scope extension unobservable without destructors; null for C input).
  if (const clang::Stmt *init = stmt->getInit())
    if (failed(emitStmt(init)))
      return failure();
  if (stmt->getConditionVariable())
    if (failed(emitStmt(stmt->getConditionVariableDeclStmt())))
      return failure();

  // Evaluate the controlling expression to an integer flag. An enum
  // condition arrives behind its integral-promotion cast; it is peeled and
  // converted with an explicit `emitrust.cast` so that the (possibly
  // unsigned) promotion type never needs to be mapped.
  Value flag;
  if (std::optional<EnumOperand> component =
          classifyEnumOperand(stmt->getCond())) {
    FailureOr<Value> value = emitEnumOperand(*component, loc);
    if (failed(value))
      return failure();
    flag = castEnumToI32(loc, *value);
  } else {
    FailureOr<Value> value = emitRValue(stmt->getCond());
    if (failed(value))
      return failure();
    flag = *value;
  }
  // An unsigned condition (`unsigned int` and wider are their own promoted
  // types; narrower unsigned types promote to plain `int` and never reach
  // here unsigned) is reinterpreted to signless i64 with an `emitrust.cast`
  // (`as i64`): ui8/ui16/ui32 values zero-extend and ui64 values keep their
  // bit pattern. The case labels below extend to the flag width with the
  // same zero-extension of their APInt bits, so the flag and every label
  // agree bit for bit even for ui64 case values above i64::MAX.
  if (isUnsignedInt(flag.getType()))
    flag = builder
               .create<emitrust::CastOp>(loc, builder.getIntegerType(64),
                                         flag)
               .getResult();
  auto flagType = llvm::dyn_cast<IntegerType>(flag.getType());
  if (!flagType)
    return emitError(loc) << "unsupported: non-integer switch condition";

  const auto *body = llvm::dyn_cast_if_present<clang::CompoundStmt>(
      stmt->getBody());
  if (!body || !isPlainSwitchBody(body))
    return emitDispatchSwitch(stmt, flag, flagType, loc);

  // Partition the body into label sections: every top-level label chain
  // (consecutive case/default labels share one target) starts a section
  // holding the statements up to the next chain. Case values are constant
  // by C semantics; clang has already checked them.
  struct Section {
    Block *block;
    SmallVector<const clang::Stmt *, 4> stmts;
  };
  SmallVector<Section> sections;
  SmallVector<llvm::APInt> caseValues;
  SmallVector<Block *> caseBlocks;
  Block *defaultBlock = nullptr;
  for (const clang::Stmt *child : body->body()) {
    const clang::Stmt *statement = child;
    if (llvm::isa<clang::SwitchCase>(child)) {
      sections.push_back({createBlock(), {}});
      while (const auto *label = llvm::dyn_cast<clang::SwitchCase>(statement)) {
        Location labelLoc = translateLoc(label->getKeywordLoc());
        if (const auto *caseStmt = llvm::dyn_cast<clang::CaseStmt>(label)) {
          if (caseStmt->getRHS())
            return emitError(labelLoc) << "unsupported: GNU case range";
          llvm::APSInt value =
              caseStmt->getLHS()->EvaluateKnownConstInt(astContext());
          caseValues.push_back(value.extOrTrunc(flagType.getWidth()));
          caseBlocks.push_back(sections.back().block);
        } else {
          defaultBlock = sections.back().block;
        }
        statement = label->getSubStmt();
      }
    }
    // `isPlainSwitchBody` guaranteed the first child starts a label chain,
    // so `sections` is never empty here, and no label of this switch hides
    // inside `statement`.
    sections.back().stmts.push_back(statement);
  }

  Block *exitBlock = createBlock();
  SmallVector<ValueRange> caseOperands(caseBlocks.size(), ValueRange());
  builder.create<cf::SwitchOp>(
      loc, flag, defaultBlock ? defaultBlock : exitBlock, ValueRange(),
      llvm::ArrayRef<llvm::APInt>(caseValues), BlockRange(caseBlocks),
      llvm::ArrayRef<ValueRange>(caseOperands));

  // Emit the sections in source order. `break` targets the exit block;
  // `continue` keeps targeting the latch of the enclosing loop, if any. A
  // section that does not end in a terminator falls through to the next
  // section (or, for the last section, to the exit block).
  loopStack.push_back(
      {exitBlock, loopStack.empty() ? nullptr : loopStack.back().continueDest});
  for (auto [index, section] : llvm::enumerate(sections)) {
    builder.setInsertionPointToEnd(section.block);
    LogicalResult sectionResult = success();
    for (const clang::Stmt *statement : section.stmts)
      if (failed(sectionResult = emitStmt(statement)))
        break;
    if (failed(sectionResult)) {
      loopStack.pop_back();
      return failure();
    }
    if (!isTerminated(builder.getInsertionBlock())) {
      Block *next =
          index + 1 < sections.size() ? sections[index + 1].block : exitBlock;
      builder.create<cf::BranchOp>(loc, next);
    }
  }
  loopStack.pop_back();

  builder.setInsertionPointToEnd(exitBlock);
  return success();
}

LogicalResult CImporter::emitDispatchSwitch(const clang::SwitchStmt *stmt,
                                            Value flag, IntegerType flagType,
                                            Location loc) {
  // Register one block per case/default label of this switch. Clang chains
  // a switch's own labels (wherever they nest inside the body) off
  // `getSwitchCaseList` in reverse source order; labels of nested switches
  // hang off their own SwitchStmt and never appear here. The list is
  // reversed so blocks and `cf.switch` case operands come out in source
  // order deterministically.
  SmallVector<const clang::SwitchCase *> labels;
  for (const clang::SwitchCase *label = stmt->getSwitchCaseList(); label;
       label = label->getNextSwitchCase())
    labels.push_back(label);
  std::reverse(labels.begin(), labels.end());

  SmallVector<llvm::APInt> caseValues;
  SmallVector<Block *> caseBlocks;
  Block *defaultBlock = nullptr;
  for (const clang::SwitchCase *label : labels) {
    Location labelLoc = translateLoc(label->getKeywordLoc());
    Block *block = createBlock();
    switchCaseBlocks[label] = block;
    if (const auto *caseStmt = llvm::dyn_cast<clang::CaseStmt>(label)) {
      if (caseStmt->getRHS())
        return emitError(labelLoc) << "unsupported: GNU case range";
      llvm::APSInt value =
          caseStmt->getLHS()->EvaluateKnownConstInt(astContext());
      caseValues.push_back(value.extOrTrunc(flagType.getWidth()));
      caseBlocks.push_back(block);
    } else {
      defaultBlock = block;
    }
  }

  Block *exitBlock = createBlock();
  SmallVector<ValueRange> caseOperands(caseBlocks.size(), ValueRange());
  builder.create<cf::SwitchOp>(
      loc, flag, defaultBlock ? defaultBlock : exitBlock, ValueRange(),
      llvm::ArrayRef<llvm::APInt>(caseValues), BlockRange(caseBlocks),
      llvm::ArrayRef<ValueRange>(caseOperands));

  // The body is emitted in source order, starting in a fresh block that is
  // reachable only if something branches into it (control enters the body
  // through the dispatch above, or through a goto). Each case/default
  // label reached during the walk redirects emission into its pre-created
  // block (the SwitchCase case of `emitStmt`), so fall-through between
  // labels — including into and out of loop bodies — is the ordinary
  // fall-into branch of an unterminated block. `break` targets the exit
  // block; `continue` keeps targeting the latch of the enclosing loop.
  // Variable places are hoisted to the entry block while the body is
  // emitted (`createVariablePlace`): the dispatch may jump over a
  // declaration, leaving the variable alive but uninitialized, exactly
  // like goto over a declaration (C11 6.2.4p6).
  builder.setInsertionPointToEnd(createBlock());
  loopStack.push_back(
      {exitBlock, loopStack.empty() ? nullptr : loopStack.back().continueDest});
  bool savedHasLabels = currentHasLabels;
  currentHasLabels = true;
  LogicalResult bodyResult = emitStmt(stmt->getBody());
  currentHasLabels = savedHasLabels;
  loopStack.pop_back();
  if (failed(bodyResult))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, exitBlock);
  builder.setInsertionPointToEnd(exitBlock);
  return success();
}

LogicalResult CImporter::emitReturnStmt(const clang::ReturnStmt *stmt) {
  Location loc = translateLoc(stmt->getReturnLoc());
  if (const clang::Expr *retValue = stmt->getRetValue()) {
    if (!currentReturnType) {
      if (currentErasedReturnBase) {
        // A classified single-global-base pointer return (CTS-S, 00089):
        // the result was erased from the signature, and the classification
        // pinned every site to `&base` (side-effect free), so the site
        // emits a bare return — no address value, no runtime state.
        builder.create<func::ReturnOp>(loc);
        builder.setInsertionPointToEnd(createBlock());
        return success();
      }
      return emitError(loc)
             << "unsupported: return with a value in a void function";
    }
    FailureOr<Value> value = failure();
    if (currentOwnerIndexReturn) {
      // Stage 1 owner-index return: the returned pointer decomposes
      // exactly like a method-call pointer argument (`emitMethodCallSite`)
      // — its i64 cursor IS the return value. `planOwners` already proved
      // every return site roots in this method's own owner class; the
      // defensive checks below mirror `emitMethodCallSite`'s.
      FailureOr<PtrExprValue> pointer = emitPointerRValue(retValue);
      if (failed(pointer))
        return failure();
      bool rootedAtOwner =
          pointer->base == currentMethodOwner ||
          llvm::isa_and_nonnull<clang::ParmVarDecl>(pointer->base);
      if (!rootedAtOwner) // Defensive; planOwners proved every site in-class.
        return emitError(loc) << "unsupported: returned pointer value does "
                                 "not root in the owner object";
      if (pointer->nonNull) // Defensive; owner planning excludes nullable
                            // regions (no i64-cursor representation).
        return emitError(loc)
               << "unsupported: possibly-null pointer returned from an "
                  "owner-index method";
      if (!pointer->cursor) // Defensive; an array base always has a cursor.
        return emitError(loc) << "unsupported: the address of a scalar "
                                 "object cannot be an owner-index return";
      value = pointer->cursor;
    } else if (isDataPointer(retValue->getType()) &&
        currentReturnType == builder.getIntegerType(64)) {
      // An integer-carrier pointer return (CTS-P3): the function's return
      // type classified to a plain i64, and every return site yields a
      // carrier value.
      value = emitCarrierValue(retValue);
    } else if (isDataPointer(retValue->getType()) &&
               llvm::isa<emitrust::FnPtrType>(currentReturnType)) {
      // A classified fn-address pointer return (CTS-P2): peel the
      // `void *` cast and emit the fn_ptr constant directly.
      const clang::Expr *fnExpr = returnedFunctionExpr(retValue);
      if (!fnExpr) // Defensive; classification pinned every return site.
        return emitError(loc) << "unsupported: returned pointer value";
      const auto *fn = llvm::cast<clang::FunctionDecl>(
          llvm::cast<clang::DeclRefExpr>(fnExpr)->getDecl());
      value = emitFunctionPointerConstant(
          fnExpr, astContext().getPointerType(fn->getType()), loc);
    } else if (isDataPointer(retValue->getType()) &&
               llvm::isa<emitrust::StructType>(currentReturnType)) {
      // FR-94: an owned FAM-record return (`return d;` in a recognized
      // allocator): the classified struct return moves the owned local out.
      // `planFamLift` pinned every return site to a claimed local or to an
      // elided-guard NULL (which never emits), so the checks below are
      // defensive nets.
      const clang::Expr *peeled = stripTrivia(retValue);
      while (const clang::Expr *sub = peelPointerCast(astContext(), peeled))
        peeled = stripTrivia(sub);
      const clang::VarDecl *owned = asLoadedLocalVarRef(peeled);
      Value ownedPlace = owned ? symbols.lookup(owned) : Value();
      if (!owned || !famAllocLocals.contains(owned) || !ownedPlace)
        return emitError(loc) << "unsupported: returned pointer value";
      value = builder
                  .create<emitrust::LoadOp>(loc, currentReturnType, ownedPlace)
                  .getResult();
    } else {
      value = emitRValue(retValue);
    }
    if (failed(value))
      return failure();
    if ((*value).getType() != currentReturnType)
      return emitError(loc) << "unsupported: return value type mismatch";
    emitCursorWritebacks(loc);
    builder.create<func::ReturnOp>(loc, *value);
  } else {
    if (currentReturnType)
      return emitError(loc)
             << "unsupported: return without a value in a non-void function";
    emitCursorWritebacks(loc);
    builder.create<func::ReturnOp>(loc);
  }
  // Continue in a fresh block; if it stays unreachable it is erased later.
  builder.setInsertionPointToEnd(createBlock());
  return success();
}

LogicalResult CImporter::emitExprStmt(const clang::Expr *expr) {
  const clang::Expr *e = expr->IgnoreParens();
  // A full expression containing a materialized temporary (e.g. the
  // `printf("%d\n", f().m)` shape, CTS 00204) is wrapped in
  // ExprWithCleanups; the "cleanup" is the end of the temp's lifetime,
  // which needs no code — unwrap so statement-position calls keep their
  // statement lowerings (the by-name printf intercept in particular).
  if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(e))
    return emitExprStmt(cleanups->getSubExpr());
  if (const auto *compound = llvm::dyn_cast<clang::CompoundAssignOperator>(e))
    return emitCompoundAssign(compound);
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e)) {
    if (binary->getOpcode() == clang::BO_Assign)
      return emitAssign(binary);
    // A comma in statement position evaluates both operands for their side
    // effects only, so a void-typed right operand is fine here.
    if (binary->getOpcode() == clang::BO_Comma) {
      if (failed(emitExprStmt(binary->getLHS())))
        return failure();
      return emitExprStmt(binary->getRHS());
    }
  }
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    if (unary->isIncrementDecrementOp())
      return emitIncDec(unary);
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e))
    return emitCallStmt(call);
  // A cast to void evaluates its operand for its side effects and discards
  // the value (C11 6.3.2.2). A side-effect-free operand needs no code at
  // all; anything else is re-entered as an expression statement, so calls,
  // assignments, and ++/-- keep their statement-position lowerings. This
  // also covers implicit ToVoid casts, e.g. the non-void arm of a
  // void-typed conditional.
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e))
    if (cast->getCastKind() == clang::CK_ToVoid) {
      if (!cast->getSubExpr()->HasSideEffects(astContext()))
        return success();
      return emitExprStmt(cast->getSubExpr());
    }
  // A void-typed conditional operator (a GNU shape: at least one arm has
  // void type) has no value to materialize, so emitConditionalOperator
  // cannot lower it; in statement position both arms are evaluated for
  // their side effects only, which is exactly an if/else.
  if (const auto *conditional = llvm::dyn_cast<clang::ConditionalOperator>(e))
    if (conditional->getType()->isVoidType())
      return emitVoidConditionalStmt(conditional);
  // A GNU statement expression in statement position (the 00214 `bla`
  // shape): the body statements run inline in the enclosing function and
  // the final expression's value is discarded — a side-effect-free final
  // expression needs no code at all, exactly like a cast to void.
  if (const auto *stmtExpr = llvm::dyn_cast<clang::StmtExpr>(e)) {
    const clang::CompoundStmt *body = stmtExpr->getSubStmt();
    const clang::Stmt *last = body->body_empty() ? nullptr : body->body_back();
    for (const clang::Stmt *child : body->body()) {
      if (child == last)
        if (const auto *lastExpr = llvm::dyn_cast<clang::Expr>(child)) {
          if (!lastExpr->HasSideEffects(astContext()))
            return success();
          return emitExprStmt(lastExpr);
        }
      if (failed(emitStmt(child)))
        return failure();
    }
    return success();
  }
  // Any other expression statement is evaluated and its value discarded.
  return success(succeeded(emitRValue(e)));
}

LogicalResult
CImporter::emitVoidConditionalStmt(const clang::ConditionalOperator *op) {
  Location loc = translateLoc(op->getQuestionLoc());
  // The constant-condition rule of `emitConditionalOperator` applies to
  // the void (statement-position) form identically: a label-free dead
  // arm is elided before lowering, while a goto-targeted label in the
  // dead arm (the 00213 kb_wait_1 shape) or a case/default label of an
  // enclosing switch keeps the FULL if/else lowering below — the
  // constant branch leaves the arm dynamically dead while its labels
  // register with the ordinary goto dispatch.
  clang::Expr::EvalResult conditionValue;
  if (op->getCond()->EvaluateAsInt(conditionValue, astContext())) {
    bool truth = conditionValue.Val.getInt() != 0;
    const clang::Expr *live = truth ? op->getTrueExpr() : op->getFalseExpr();
    const clang::Expr *dead = truth ? op->getFalseExpr() : op->getTrueExpr();
    if (!containsLabelStmt(dead) && !findNestedSwitchLabel(dead))
      return emitExprStmt(live);
  }
  FailureOr<Value> condition = emitCondition(op->getCond());
  if (failed(condition))
    return failure();

  Block *thenBlock = createBlock();
  Block *elseBlock = createBlock();
  Block *contBlock = createBlock();
  builder.create<cf::CondBranchOp>(loc, *condition, thenBlock, ValueRange(),
                                   elseBlock, ValueRange());

  builder.setInsertionPointToEnd(thenBlock);
  if (failed(emitExprStmt(op->getTrueExpr())))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, contBlock);

  builder.setInsertionPointToEnd(elseBlock);
  if (failed(emitExprStmt(op->getFalseExpr())))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, contBlock);

  builder.setInsertionPointToEnd(contBlock);
  return success();
}

LogicalResult CImporter::emitAssign(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  // FR-94: a whole-record assignment of an ADMITTED FAM record has no
  // representation: C's FAM assignment copies the FIELDS and NOT the tail
  // (the destination keeps its own tail storage), while every Rust spelling
  // of the owned-Vec representation either MOVES or CLONES the tail — no
  // rendering reproduces C, so the site rejects rather than silently
  // diverging. Non-admitted FAM records (gap layouts, non-u8 tails) have no
  // Vec field and keep their historical fields-only Copy semantics.
  if (const clang::RecordDecl *record =
          op->getLHS()->getType()->getAsRecordDecl();
      record && famTailField(record))
    return emitError(loc) << "unsupported: whole-record assignment of a "
                             "flexible-array-member record";
  // Reassigning a FILE* handle local (`f = fopen(...)` after fclose, the
  // serial-reuse shape of 00187) stores a fresh handle into its owned
  // place. Non-handle FILE* destinations fall through to the historical
  // pointer paths and their located rejections.
  if (isFilePtrType(op->getLHS()->getType()))
    if (const clang::VarDecl *var = asVarRef(op->getLHS()))
      if (Value place = fileLocals.lookup(var))
        return emitFileOpenInto(place, op->getRHS());
  // Rebinding a decomposed pointer local (or a slice-classified pointer
  // parameter) recomputes its cursor; no pointer value is ever
  // materialized. Function pointers are ordinary values and take the
  // plain place-assignment (or global-store) path below.
  if (isPointerType(op->getLHS()->getType()) &&
      !isFunctionPointer(op->getLHS()->getType())) {
    if (const clang::VarDecl *var = asVarRef(op->getLHS()))
      if (pointerLocals.contains(var) || pointerRegions.tracks(var) ||
          pointerPointerLocals.contains(var) ||
          pointerRegions.tracksSecondOrder(var))
        return storePointerAssign(loc, var, op->getRHS());
    // A pointer-typed global rebinds by storing its global cursor.
    if (const clang::VarDecl *global = asGlobalDataPointerRef(op->getLHS()))
      if (pointerGlobals.contains(global->getCanonicalDecl()))
        return storePointerAssign(loc, global, op->getRHS());
    // `*pp = rhs`: re-pointing through a second-order pointer is exactly
    // an assignment to the first-order pointer it selects (CTS-P5).
    if (const clang::VarDecl *pp = secondOrderDerefVar(op->getLHS())) {
      auto it = pointerPointerLocals.find(pp);
      if (it == pointerPointerLocals.end())
        return emitError(loc) << "unsupported: pointer-to-pointer variable '"
                              << pp->getName()
                              << "' has no bound pointer variable";
      return storePointerAssign(loc, it->second, op->getRHS());
    }
    // `*endp = rhs` on a Shape-P paired out-cursor parameter (C99-43
    // slice 1b): the unique unconditional write assigns the RHS's
    // cursor value (in the co-parameter's slice coordinates) straight
    // through the `&mut i64` argument — no cell, no return-site
    // writeback. Checked ahead of the Shape-S branch below: a P
    // parameter has no pointer-local binding.
    if (const clang::ParmVarDecl *pairedParam =
            asPointerPointerParamDeref(op->getLHS());
        pairedParam && pairedCursorParams.contains(pairedParam))
      return emitPairedCursorWrite(pairedParam, op->getRHS(), loc);
    // `*efp = rhs` on a Shape-G single-global-or-NULL out-param cursor
    // (C99-43 C1): the unique unconditional write assigns Some(0)/None
    // through the `&mut Option<i64>` cell (the ternary form branches
    // and assigns per arm). Like Shape P, a G parameter has no
    // pointer-local binding.
    if (const clang::ParmVarDecl *globalParam =
            asPointerPointerParamDeref(op->getLHS());
        globalParam && globalCursorParams.contains(globalParam))
      return emitGlobalCursorWrite(globalParam, op->getRHS(), loc);
    // `*s = rhs` on a Shape-S cursor parameter (CTS 00204): the
    // advancement writes the parameter's cursor cell; the return-site
    // writebacks make it visible to the caller.
    if (const clang::ParmVarDecl *cursorParam =
            asPointerPointerParamDeref(op->getLHS());
        cursorParam && pointerLocals.contains(cursorParam))
      return storePointerAssign(loc, cursorParam, op->getRHS());
    // A data-pointer struct member holds a statically resolved degenerate
    // binding; the write validates against it and emits nothing (CTS-P2).
    if (dataPointerFieldOf(op->getLHS()))
      return emitMemberPointerAssign(
          llvm::cast<clang::MemberExpr>(stripTrivia(op->getLHS())),
          op->getRHS(), loc);
    return emitError(loc)
           << "unsupported: assignment to this pointer expression";
  }
  // CTS-BR (00216): fn-ptr TABLE slots are never reassigned after their
  // initializer — the folded Some(target) element list is a static fact.
  if (isFunctionPointer(op->getLHS()->getType()))
    if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(
            stripTrivia(op->getLHS())))
      if (subscript->getBase()
              ->IgnoreParenImpCasts()
              ->getType()
              .getCanonicalType()
              ->isArrayType())
        return emitError(loc)
               << "unsupported: assignment to a function-pointer array "
                  "element";
  // CTS-BR (00216): whole-aggregate assignment over byte-region records
  // is a per-byte region copy when both sides are designators; other
  // right-hand sides (calls) keep the whole-value paths below.
  if (op->getLHS()->getType().getCanonicalType()->isRecordType() &&
      isByteRegionAggregate(op->getLHS()->getType()) &&
      isByteRegionDesignator(op->getLHS()) &&
      isByteRegionDesignator(op->getRHS()))
    return emitByteRegionAggregateAssign(op);
  // Whole-value store to a global in statement position: a direct
  // emitrust.global_store, no staging copy needed. Value-position uses go
  // through emitAssignToPlace, whose staged copy provides the place the
  // surrounding expression loads from.
  if (const clang::VarDecl *var = asDirectGlobalRef(op->getLHS())) {
    const GlobalInfo &global = globals.find(var)->second;
    FailureOr<Value> value = emitRValue(op->getRHS());
    if (failed(value))
      return failure();
    if ((*value).getType() != global.type)
      return emitError(loc)
             << "unsupported: assigned value type does not match the variable";
    builder.create<emitrust::GlobalStoreOp>(loc, *value,
                                            globalSymbol(global.symbol));
    return success();
  }
  // An element write through a cell-slice parameter (CTS-P10) is an
  // `emitrust.cell_set` on the reference itself; value-position uses keep
  // a located rejection in emitAssignToPlace.
  if (std::optional<CellSliceAccess> access =
          matchCellSliceAccess(op->getLHS()))
    return emitCellSliceAssign(*access, op->getRHS(), loc);
  // A simple store to a bit-field member is the C99-45 read-modify-write
  // accessor; statement position discards the field value.
  if (const auto *memberExpr =
          llvm::dyn_cast<clang::MemberExpr>(stripTrivia(op->getLHS())))
    if (const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(memberExpr->getMemberDecl()))
      if (field->isBitField())
        return success(succeeded(emitBitFieldAssign(
            memberExpr, op->getRHS(), loc, assignStalenessRisk(op),
            /*wantValue=*/false)));
  return success(succeeded(emitAssignToPlace(op)));
}

FailureOr<Value>
CImporter::emitAssignToPlace(const clang::BinaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  // A decomposed pointer has no place to re-load the assigned value from;
  // a function pointer is an ordinary value with an ordinary place.
  if (isPointerType(op->getLHS()->getType()) &&
      !isFunctionPointer(op->getLHS()->getType()))
    return emitError(loc)
           << "unsupported: pointer assignment in value position";
  if (matchCellSliceAccess(op->getLHS()))
    return emitError(loc) << "unsupported: assignment through a cell-slice "
                             "parameter in value position";
  // A value-position store to a bit-field member: the C99-45
  // read-modify-write accessor also stages the truncated post-store field
  // value (C's value of an assignment), returned as a re-loadable place.
  if (const auto *memberExpr =
          llvm::dyn_cast<clang::MemberExpr>(stripTrivia(op->getLHS())))
    if (const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(memberExpr->getMemberDecl()))
      if (field->isBitField())
        return emitBitFieldAssign(memberExpr, op->getRHS(), loc,
                                  assignStalenessRisk(op), /*wantValue=*/true);
  // A store through a wider-than-element view over a byte region
  // (CTS-P11) widens to a `to_ne_bytes` store over sizeof(T) consecutive
  // bytes; the assignment's value is staged in a temporary so a value
  // position can re-load it.
  if (ByteViewDeref wide = classifyByteViewDeref(op->getLHS());
      wide.wideByte) {
    GlobalWriteback writeback;
    FailureOr<WideByteAccess> access =
        resolveWideByteAccess(wide, loc, &writeback);
    if (failed(access))
      return failure();
    FailureOr<Value> value = emitRValue(op->getRHS());
    if (failed(value))
      return failure();
    Value stored = *value;
    if (stored.getType() != access->valueType) {
      FailureOr<Value> converted =
          convertScalarValue(loc, stored, access->valueType);
      if (failed(converted))
        return failure();
      stored = *converted;
    }
    if (failed(commitGlobalWriteback(
            loc, writeback, assignStalenessRisk(op), [&]() {
              return emitWideByteStore(*access, stored, loc);
            })))
      return failure();
    Value staged = createVariablePlace(loc, access->valueType);
    builder.create<emitrust::AssignOp>(loc, staged, stored);
    return staged;
  }
  // FR-83: a store to an integer-scalar leaf through an opaque-union ARM
  // scatters the value over the blob byte view at the leaf's
  // clang-computed offset (`to_ne_bytes`; a single blob subscript for one
  // byte). Same staging contract as the wide byte view above: the
  // assigned value is re-loadable from a temporary, and a staged global
  // base commits through the writeback.
  if (isOpaqueArmScalarLeaf(op->getLHS())) {
    GlobalWriteback writeback;
    FailureOr<WideByteAccess> access =
        resolveOpaqueArmByteView(op->getLHS(), loc, &writeback);
    if (failed(access))
      return failure();
    FailureOr<Value> value = emitRValue(op->getRHS());
    if (failed(value))
      return failure();
    Value stored = *value;
    if (stored.getType() != access->valueType) {
      FailureOr<Value> converted =
          convertScalarValue(loc, stored, access->valueType);
      if (failed(converted))
        return failure();
      stored = *converted;
    }
    if (failed(commitGlobalWriteback(
            loc, writeback, assignStalenessRisk(op), [&]() {
              return emitOpaqueArmStore(*access, stored, loc);
            })))
      return failure();
    Value staged = createVariablePlace(loc, access->valueType);
    builder.create<emitrust::AssignOp>(loc, staged, stored);
    return staged;
  }
  GlobalWriteback writeback;
  FailureOr<Value> place = emitLValue(op->getLHS(), &writeback);
  if (failed(place))
    return failure();
  // The destination's value type positions the right-hand side: a
  // refined (callsite-inferred, FR-29 / CTS 00209) fn-ptr place rebinds
  // function references against its refined signature; every other
  // destination takes the ordinary rvalue path unchanged.
  Type assignedType;
  if (auto lvalueType =
          llvm::dyn_cast<emitrust::LValueType>((*place).getType()))
    assignedType = lvalueType.getValueType();
  FailureOr<Value> value = emitPositionedRValue(assignedType, op->getRHS());
  if (failed(value))
    return failure();
  // A store through a union pun arm lands the bit-exactly reinterpreted
  // (slot-typed) value on the slot.
  FailureOr<Value> stored =
      reinterpretUnionArmWrite(op->getLHS(), *value, loc);
  if (failed(stored))
    return failure();
  Value toStore = *stored;
  // A store through a same-width integer view (`*(unsigned int *)p = u`
  // over an int base, CTS-P9) casts the value back to the base element
  // type before assigning: the place is the base element's own place.
  if (classifyByteViewDeref(op->getLHS()).reinterpreted)
    if (auto lvalueType =
            llvm::dyn_cast<emitrust::LValueType>((*place).getType()))
      if (lvalueType.getValueType() != toStore.getType())
        toStore = builder
                      .create<emitrust::CastOp>(loc, lvalueType.getValueType(),
                                                toStore)
                      .getResult();
  if (failed(commitGlobalWriteback(
          loc, writeback, assignStalenessRisk(op),
          [&]() { return storeToPlace(loc, *place, toStore); })))
    return failure();
  return place;
}

LogicalResult
CImporter::emitCompoundAssign(const clang::CompoundAssignOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  // `p += n` / `p -= n` on a decomposed pointer local is cursor arithmetic.
  if (isPointerType(op->getLHS()->getType()))
    return emitPointerCompoundAssign(op);
  // Compound assignment to a whole global in statement position:
  // load-modify-store through the global access ops, no staging copy
  // needed. Value-position uses go through emitCompoundAssignToPlace.
  if (const clang::VarDecl *var = asDirectGlobalRef(op->getLHS())) {
    const GlobalInfo &global = globals.find(var)->second;
    Value current = builder
                        .create<emitrust::GlobalLoadOp>(
                            loc, global.type, globalSymbol(global.symbol))
                        .getResult();
    FailureOr<Value> result = buildCompoundAssignValue(loc, op, current);
    if (failed(result))
      return failure();
    builder.create<emitrust::GlobalStoreOp>(loc, *result,
                                            globalSymbol(global.symbol));
    return success();
  }
  return success(succeeded(emitCompoundAssignToPlace(op)));
}

FailureOr<Value>
CImporter::emitCompoundAssignToPlace(const clang::CompoundAssignOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  // A decomposed pointer has no place to re-load the assigned value from.
  if (isPointerType(op->getLHS()->getType()))
    return emitError(loc)
           << "unsupported: pointer assignment in value position";
  if (matchCellSliceAccess(op->getLHS()))
    return emitError(loc) << "unsupported: compound assignment through a "
                             "cell-slice parameter";
  // A compound assignment through a wide byte view (CTS-P11) is a
  // read-modify-write over the same sizeof(T)-byte window: from_ne_bytes
  // load, computation, to_ne_bytes store (and, over a global byte region,
  // the staged copy's writeback).
  if (ByteViewDeref wide = classifyByteViewDeref(op->getLHS());
      wide.wideByte) {
    GlobalWriteback writeback;
    FailureOr<WideByteAccess> access =
        resolveWideByteAccess(wide, loc, &writeback);
    if (failed(access))
      return failure();
    FailureOr<Value> current = emitWideByteLoad(*access, loc);
    if (failed(current))
      return failure();
    FailureOr<Value> result = buildCompoundAssignValue(loc, op, *current);
    if (failed(result))
      return failure();
    if (failed(commitGlobalWriteback(
            loc, writeback, assignStalenessRisk(op), [&]() {
              return emitWideByteStore(*access, *result, loc);
            })))
      return failure();
    Value staged = createVariablePlace(loc, access->valueType);
    builder.create<emitrust::AssignOp>(loc, staged, *result);
    return staged;
  }
  // FR-83: compound assignment through an opaque-union arm leaf is a
  // read-modify-write over the same blob byte view — one widened load,
  // the computation at Sema's type, one widened store (and, over a staged
  // global base, the writeback).
  if (isOpaqueArmScalarLeaf(op->getLHS())) {
    GlobalWriteback writeback;
    FailureOr<WideByteAccess> access =
        resolveOpaqueArmByteView(op->getLHS(), loc, &writeback);
    if (failed(access))
      return failure();
    FailureOr<Value> current = emitOpaqueArmLoad(*access, loc);
    if (failed(current))
      return failure();
    FailureOr<Value> result = buildCompoundAssignValue(loc, op, *current);
    if (failed(result))
      return failure();
    if (failed(commitGlobalWriteback(
            loc, writeback, assignStalenessRisk(op), [&]() {
              return emitOpaqueArmStore(*access, *result, loc);
            })))
      return failure();
    Value staged = createVariablePlace(loc, access->valueType);
    builder.create<emitrust::AssignOp>(loc, staged, *result);
    return staged;
  }
  GlobalWriteback writeback;
  FailureOr<Value> place = emitLValue(op->getLHS(), &writeback);
  if (failed(place))
    return failure();
  Value current = loadPlace(loc, *place);
  // A union pun arm's place is its slot's: the computation happens on the
  // arm's own type, so the loaded slot value reinterprets to the arm
  // (bit-exact) and the computed result reinterprets back before the
  // store — without this a float arm over an integer slot would
  // VALUE-convert through `convertScalarValue` instead.
  FailureOr<Value> loaded = reinterpretUnionArmRead(op->getLHS(), current, loc);
  if (failed(loaded))
    return failure();
  FailureOr<Value> result = buildCompoundAssignValue(loc, op, *loaded);
  if (failed(result))
    return failure();
  FailureOr<Value> stored = reinterpretUnionArmWrite(op->getLHS(), *result, loc);
  if (failed(stored))
    return failure();
  if (failed(commitGlobalWriteback(
          loc, writeback, assignStalenessRisk(op),
          [&]() { return storeToPlace(loc, *place, *stored); })))
    return failure();
  return place;
}

FailureOr<Value> CImporter::buildCompoundAssignValue(
    Location loc, const clang::CompoundAssignOperator *op, Value current) {
  Type storedType = current.getType();
  FailureOr<Type> computeType = mapType(op->getComputationLHSType(), loc);
  if (failed(computeType))
    return failure();
  // `char/short x; x += wider;`: Sema records the promoted type the
  // operation happens at; widen the loaded LHS to it (a no-op when no
  // promotion applies).
  FailureOr<Value> widened = convertScalarValue(loc, current, *computeType);
  if (failed(widened))
    return failure();
  FailureOr<Value> rhs = emitRValue(op->getRHS());
  if (failed(rhs))
    return failure();
  clang::BinaryOperatorKind opcode =
      clang::BinaryOperator::getOpForCompoundAssignment(op->getOpcode());
  Value rhsValue = *rhs;
  // The shift amount's C type is independent of the shifted operand's, so
  // `<<=`/`>>=` normalize the right operand to the (widened) left
  // operand's width; every other compound assignment meets its RHS at the
  // computation type Sema already converted it to.
  auto lhsInt = llvm::dyn_cast<IntegerType>((*widened).getType());
  auto rhsInt = llvm::dyn_cast<IntegerType>(rhsValue.getType());
  if ((opcode == clang::BO_Shl || opcode == clang::BO_Shr) && lhsInt && rhsInt)
    rhsValue = castToIntType(loc, rhsValue, lhsInt);
  if ((*widened).getType() != rhsValue.getType())
    return emitError(loc)
           << "unsupported: compound assignment operand type mismatch";
  FailureOr<Value> result = buildBinaryArith(loc, opcode, *widened, rhsValue);
  if (failed(result))
    return failure();
  // C converts the computed value back to the LHS type before storing
  // (C99 6.5.16.2p3 via 6.5.16.1p2).
  return convertScalarValue(loc, *result, storedType);
}

FailureOr<Value> CImporter::convertScalarValue(Location loc, Value value,
                                               Type target) {
  Type source = value.getType();
  if (source == target)
    return value;
  auto sourceInt = llvm::dyn_cast<IntegerType>(source);
  auto targetInt = llvm::dyn_cast<IntegerType>(target);
  // C converts to `_Bool` by comparison against zero, not by truncation;
  // reject rather than lower it wrong.
  if ((sourceInt && sourceInt.getWidth() == 1) ||
      (targetInt && targetInt.getWidth() == 1))
    return emitError(loc) << "unsupported: _Bool conversion";
  if (sourceInt && targetInt)
    return castToIntType(loc, value, targetInt);
  auto sourceFloat = llvm::dyn_cast<FloatType>(source);
  auto targetFloat = llvm::dyn_cast<FloatType>(target);
  if (sourceFloat && targetFloat) {
    if (sourceFloat.getWidth() < targetFloat.getWidth())
      return builder.create<arith::ExtFOp>(loc, targetFloat, value)
          .getResult();
    return builder.create<arith::TruncFOp>(loc, targetFloat, value)
        .getResult();
  }
  if (sourceInt && targetFloat) {
    // Unsigned to float is an `emitrust.cast`: Rust's `u* as f*` performs
    // the same round-to-nearest conversion as C.
    if (sourceInt.isUnsigned())
      return builder.create<emitrust::CastOp>(loc, target, value).getResult();
    return builder.create<arith::SIToFPOp>(loc, target, value).getResult();
  }
  if (sourceFloat && targetInt) {
    // Float to unsigned is an `emitrust.cast`; Rust's `as` saturates where
    // C is undefined, an acceptable defined refinement (matching the
    // `CK_FloatingToIntegral` lowering).
    if (targetInt.isUnsigned())
      return builder.create<emitrust::CastOp>(loc, target, value).getResult();
    return builder.create<arith::FPToSIOp>(loc, target, value).getResult();
  }
  return emitError(loc) << "unsupported scalar conversion";
}

LogicalResult CImporter::emitIncDec(const clang::UnaryOperator *op) {
  // `p++` / `--p` on a decomposed pointer local walks its cursor; the
  // pointer value form of the expression is discarded in statement position.
  if (isPointerType(op->getSubExpr()->getType()))
    return success(succeeded(emitPointerRValue(op)));
  return success(succeeded(emitIncDecValue(op)));
}

FailureOr<Value> CImporter::emitIncDecValue(const clang::UnaryOperator *op) {
  Location loc = translateLoc(op->getOperatorLoc());
  // Pointer ++/-- value forms are consumed by `emitPointerRValue`; a
  // pointer value reaching this scalar path has no representation.
  if (isPointerType(op->getSubExpr()->getType()))
    return emitError(loc) << "unsupported pointer expression in this context";
  // ++/-- on a whole global: load-modify-store through the global access
  // ops, no staging copy needed.
  if (const clang::VarDecl *var = asDirectGlobalRef(op->getSubExpr())) {
    const GlobalInfo &global = globals.find(var)->second;
    auto intType = llvm::dyn_cast<IntegerType>(global.type);
    if (!intType)
      return emitError(loc) << "unsupported: ++/-- on a non-integer operand";
    Value current = builder
                        .create<emitrust::GlobalLoadOp>(
                            loc, global.type, globalSymbol(global.symbol))
                        .getResult();
    // createScalarIntConstant/buildBinaryArith cover both the signless
    // (arith) and unsigned (emitrust) domains.
    Value one = createScalarIntConstant(loc, intType, 1);
    clang::BinaryOperatorKind opcode =
        op->isIncrementOp() ? clang::BO_Add : clang::BO_Sub;
    FailureOr<Value> next = buildBinaryArith(loc, opcode, current, one);
    if (failed(next))
      return failure();
    builder.create<emitrust::GlobalStoreOp>(loc, *next,
                                            globalSymbol(global.symbol));
    // C evaluates postfix forms to the original value and prefix forms to
    // the updated one.
    return op->isPostfix() ? current : *next;
  }
  // FR-83: ++/-- on an integer-scalar leaf through an opaque-union ARM is
  // a read-modify-write over the blob byte view (the composed
  // load/store images; a one-byte leaf never stages ne_bytes).
  if (isOpaqueArmScalarLeaf(op->getSubExpr())) {
    GlobalWriteback writeback;
    FailureOr<WideByteAccess> access =
        resolveOpaqueArmByteView(op->getSubExpr(), loc, &writeback);
    if (failed(access))
      return failure();
    FailureOr<Value> current = emitOpaqueArmLoad(*access, loc);
    if (failed(current))
      return failure();
    Value one = createScalarIntConstant(loc, access->valueType, 1);
    clang::BinaryOperatorKind opcode =
        op->isIncrementOp() ? clang::BO_Add : clang::BO_Sub;
    FailureOr<Value> next = buildBinaryArith(loc, opcode, *current, one);
    if (failed(next))
      return failure();
    if (failed(commitGlobalWriteback(
            loc, writeback, op->getSubExpr()->HasSideEffects(astContext()),
            [&]() { return emitOpaqueArmStore(*access, *next, loc); })))
      return failure();
    // C evaluates postfix forms to the original value and prefix forms
    // to the updated one.
    return op->isPostfix() ? *current : *next;
  }
  GlobalWriteback writeback;
  FailureOr<Value> place = emitLValue(op->getSubExpr(), &writeback);
  if (failed(place))
    return failure();
  Value current = loadPlace(loc, *place);
  // A union pun arm's place is its slot's: reinterpret the loaded slot
  // value to the arm's own type first, so ++/-- on a float arm over an
  // integer slot is the (already rejected) non-integer case rather than
  // raw arithmetic on the bit pattern; an integer pun arm computes at
  // its own signedness and reinterprets back before the store.
  FailureOr<Value> loaded =
      reinterpretUnionArmRead(op->getSubExpr(), current, loc);
  if (failed(loaded))
    return failure();
  current = *loaded;
  auto intType = llvm::dyn_cast<IntegerType>(current.getType());
  if (!intType)
    return emitError(loc) << "unsupported: ++/-- on a non-integer operand";
  Value one = createScalarIntConstant(loc, intType, 1);
  clang::BinaryOperatorKind opcode =
      op->isIncrementOp() ? clang::BO_Add : clang::BO_Sub;
  FailureOr<Value> next = buildBinaryArith(loc, opcode, current, one);
  if (failed(next))
    return failure();
  FailureOr<Value> stored =
      reinterpretUnionArmWrite(op->getSubExpr(), *next, loc);
  if (failed(stored))
    return failure();
  // The subexpression's own side effects (a subscript-index call,
  // `g[f()]++`) run after the staging load and force the pre-store
  // refresh of the staged copy.
  if (failed(commitGlobalWriteback(
          loc, writeback, op->getSubExpr()->HasSideEffects(astContext()),
          [&]() { return storeToPlace(loc, *place, *stored); })))
    return failure();
  // C evaluates postfix forms to the original value and prefix forms to
  // the updated one.
  return op->isPostfix() ? current : *next;
}

LogicalResult CImporter::emitCallStmt(const clang::CallExpr *call) {
  const clang::FunctionDecl *callee = call->getDirectCallee();
  // va_start/va_end inside a monomorphization clone (CTS 00204):
  // va_start resets the internal consumption cursor; va_end is a no-op.
  // Outside a clone both are unreachable (clang only admits them in
  // variadic definitions, and every va_list-using definition either
  // monomorphizes or rejects), so the guard is defensive.
  if (callee) {
    switch (callee->getBuiltinID()) {
    case clang::Builtin::BI__builtin_va_start:
    case clang::Builtin::BI__builtin_c23_va_start:
    case clang::Builtin::BI__va_start:
    case clang::Builtin::BIva_start: {
      Location loc = translateLoc(call->getBeginLoc());
      if (!currentVaCloneActive)
        return emitError(loc)
               << "unsupported: va_start outside a variadic definition";
      Value zero =
          createIntConstant(loc, builder.getIntegerType(64), 0);
      builder.create<memref::StoreOp>(loc, zero, currentVaCursorCell);
      return success();
    }
    case clang::Builtin::BI__builtin_va_end:
    case clang::Builtin::BIva_end:
      if (!currentVaCloneActive)
        return emitError(translateLoc(call->getBeginLoc()))
               << "unsupported: va_end outside a variadic definition";
      return success();
    default:
      break;
    }
  }
  if (callee && callee->getDeclName().isIdentifier()) {
    llvm::StringRef name = callee->getName();
    // printf/puts/putchar are intercepted by name only when the project
    // supplies no definition of its own; a user-defined printf (any
    // signature — <stdio.h> is not imported) or puts/putchar is an
    // ordinary call to the imported definition.
    if (name == "printf" && !callee->getDefinition())
      return emitPrintf(call);
    if (name == "puts" && !callee->getDefinition())
      return emitPuts(call);
    if (name == "putchar" && !callee->getDefinition())
      return emitPutchar(call);
    // Hosted <string.h> copy/fill functions (design.md C99-48, CTS-L1) are
    // lowered by name in statement position when the project supplies no
    // definition of its own; C's pointer result (the destination) has no
    // decomposed representation, so value uses keep located rejections in
    // emitCall.
    if (!callee->getDefinition()) {
      // A statement-position `fclose(f)` drops the owned handle (C99-48);
      // its int result has no representation, so value uses keep a
      // located rejection in emitCall.
      if (name == "fclose")
        return emitFileClose(call);
      if (name == "strcpy")
        return emitStringCopyCall(call, "strcpy", /*hasCount=*/false);
      if (name == "strncpy")
        return emitStringCopyCall(call, "strncpy", /*hasCount=*/true);
      if (name == "strcat")
        return emitStringCopyCall(call, "strcat", /*hasCount=*/false);
      if (name == "memset")
        return emitMemsetCall(call);
      if (name == "memcpy")
        return emitMemcpyCall(call, "memcpy");
      // memmove shares memcpy's lowering exactly: distinct char regions
      // never overlap, and the same-object shape already goes through
      // `copy_within`, which is memmove's overlap-correct copy.
      if (name == "memmove")
        return emitMemcpyCall(call, "memmove");
      // A statement-position `exit(status)` terminates the process with
      // C's exit-status semantics (design.md C99-48).
      if (name == "exit")
        return emitExitCall(call);
      // A statement-position `free(p)` whose argument roots in a recognized
      // local heap allocation (W4.2e Part A) is a no-op: the synthesized
      // backing array drops at function scope end, and a defined program
      // never reads freed storage (the differential is the oracle). `free`
      // of any other pointer rejects located — the region model has no
      // deallocation for it. A value use of `free`'s int result keeps its
      // located rejection in emitCall (it is never intercepted there).
      if (name == "free") {
        Location freeLoc = translateLoc(call->getBeginLoc());
        const clang::Expr *arg = stripTrivia(call->getArg(0));
        while (const clang::Expr *peeled = peelPointerCast(astContext(), arg))
          arg = stripTrivia(peeled);
        if (const clang::VarDecl *root = asLoadedLocalVarRef(arg)) {
          // FR-64: `free` of a lifted `String` local is a no-op — the
          // `String` drops at scope end, exactly the deallocation `free`
          // denotes (escape/return are rejected, so ownership is single).
          if (stringFillLocals.contains(root))
            return success();
          // FR-65: `free` of a lifted `Vec<T>` local is a no-op — the `Vec`
          // drops at scope end (single owner: escape/return are rejected).
          if (vecValueLocals.contains(root))
            return success();
          // FR-94: `free` of an owned FAM-record local is a no-op — the
          // struct (and its Vec tail) drops at scope end, or moved into the
          // owned free wrapper earlier (in which case this free is the
          // wrapper's own and never emits here).
          if (famAllocLocals.contains(root))
            return success();
          auto it = pointerLocals.find(root);
          if (it != pointerLocals.end() && it->second.backing)
            return success();
        }
        // FR-94: `free(param)` inside the free-only wrapper — the parameter
        // is OWNED BY VALUE (`ParamKind::OwnedRecord`), so dropping it at
        // scope end IS the deallocation and the free itself is a no-op.
        // (`asLoadedLocalVarRef` excludes parameters, hence the own peel.)
        {
          const clang::Expr *paramArg = stripTrivia(arg);
          if (const auto *cast =
                  llvm::dyn_cast<clang::ImplicitCastExpr>(paramArg);
              cast && cast->getCastKind() == clang::CK_LValueToRValue)
            paramArg = stripTrivia(cast->getSubExpr());
          if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(paramArg))
            if (const auto *parm =
                    llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl());
                parm && famOwnedParams.contains(parm))
              return success();
        }
        // FR-96: `free(base->field)` on a lifted member-held FAM record
        // (heatshrink_encoder_free's `HEATSHRINK_FREE(hse->search_index,
        // ...)`, encoder.c:110) — the member owns its payload, so storing
        // `None` drops it, which IS the deallocation. A member-read local
        // (`free(hsi)`) frees the same owned payload and routes
        // identically.
        if (const clang::MemberExpr *member = famOptionMemberOf(arg)) {
          const auto *field =
              llvm::cast<clang::FieldDecl>(member->getMemberDecl());
          FailureOr<Value> place =
              emitFamOptionMemberPlace(member, field, freeLoc,
                                       /*writeback=*/nullptr);
          if (failed(place))
            return failure();
          FailureOr<emitrust::OpaqueType> optionType =
              famOptionMemberType(field, freeLoc);
          if (failed(optionType))
            return failure();
          Value none = builder
                           .create<emitrust::LiteralOp>(
                               freeLoc, *optionType,
                               builder.getStringAttr("None"))
                           .getResult();
          builder.create<emitrust::AssignOp>(freeLoc, *place, none);
          return success();
        }
        return emitError(freeLoc)
               << "unsupported: free of a pointer not rooted in a "
                  "recognized allocation";
      }
    }
  }
  // A statement-position call through a devirtualized alias of a hosted
  // variadic (CTS-S, 00189) routes through the printf machinery — the
  // fprintf shape swallows its leading `stdout` argument. Non-variadic
  // aliases fall through to emitCall's direct-call devirtualization.
  if (const clang::FunctionDecl *target = devirtualizedCallee(call))
    if (target->isVariadic())
      return emitAliasedPrintf(call, target);
  // Calls without a direct callee (function pointers) are handled by the
  // indirect path inside emitCall.
  return success(succeeded(emitCall(call)));
}

void CImporter::emitPrintMacro(Location loc, std::string rustFormat,
                               ValueRange operands) {
  // A newline-terminated format folds its trailing `\n` into `println!`,
  // which writes byte-for-byte the same stdout as `print!` of the original
  // string (clippy::print_with_newline). A format without a trailing newline
  // keeps `print!`.
  StringRef macro = "print!";
  if (!rustFormat.empty() && rustFormat.back() == '\n') {
    rustFormat.pop_back();
    macro = "println!";
  }
  // A bare `println!()` (the whole format was a lone newline, no holes)
  // renders from an empty args array; `println!("")` would trip
  // clippy::println_empty_string.
  if (rustFormat.empty() && operands.empty()) {
    builder.create<emitrust::CallOpaqueOp>(
        loc, TypeRange(), builder.getStringAttr(macro),
        builder.getArrayAttr({}), ValueRange());
    return;
  }
  SmallVector<Attribute> callArguments;
  callArguments.push_back(builder.getStringAttr(rustFormat));
  for (unsigned i = 0, e = operands.size(); i < e; ++i)
    callArguments.push_back(builder.getIndexAttr(i));
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr(macro),
      builder.getArrayAttr(callArguments), operands);
}

LogicalResult
CImporter::emitAliasedPrintf(const clang::CallExpr *call,
                             const clang::FunctionDecl *target) {
  Location loc = translateLoc(call->getBeginLoc());
  unsigned formatIndex = 0;
  if (target->getDeclName().isIdentifier() &&
      target->getName() == "fprintf") {
    if (call->getNumArgs() == 0)
      return emitError(loc) << "unsupported: fprintf without a stream "
                               "argument";
    // The swallowed stream slot (the fprintf->printf routing): the ONLY
    // position where a FILE* value is accepted, and only as the literal
    // `stdout`. Every other FILE* use keeps its located rejection.
    const auto *stream = llvm::dyn_cast<clang::DeclRefExpr>(
        call->getArg(0)->IgnoreParenImpCasts());
    const clang::NamedDecl *streamDecl =
        stream ? llvm::dyn_cast<clang::NamedDecl>(stream->getDecl())
               : nullptr;
    if (!streamDecl || !streamDecl->getDeclName().isIdentifier() ||
        canonicalStreamName(streamDecl->getName()) != "stdout")
      return emitError(translateLoc(call->getArg(0)->getBeginLoc()))
             << "unsupported: a devirtualized fprintf call requires the "
                "literal 'stdout' stream argument";
    formatIndex = 1;
  }
  if (call->getNumArgs() <= formatIndex)
    return emitError(loc) << "unsupported: printf without a format string";
  const clang::Expr *formatExpr =
      call->getArg(formatIndex)->IgnoreParenImpCasts();
  const auto *literal = llvm::dyn_cast<clang::StringLiteral>(formatExpr);
  if (!literal || !literal->isOrdinary())
    return emitError(loc)
           << "unsupported: printf format must be an ordinary string literal";

  SmallVector<Value> operands;
  FailureOr<std::string> rustFormat = translatePrintfFormat(
      loc, call, literal, /*firstArgIndex=*/formatIndex + 1, operands);
  if (failed(rustFormat))
    return failure();

  emitPrintMacro(loc, *rustFormat, operands);
  return success();
}

LogicalResult CImporter::emitPrintf(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() == 0)
    return emitError(loc) << "unsupported: printf without a format string";
  const clang::Expr *formatExpr = call->getArg(0)->IgnoreParenImpCasts();
  const auto *literal = llvm::dyn_cast<clang::StringLiteral>(formatExpr);
  if (!literal || !literal->isOrdinary())
    return emitError(loc)
           << "unsupported: printf format must be an ordinary string literal";

  SmallVector<Value> operands;
  // C99-43 C3: the stdout `print!` context is the only caller that permits an
  // argv-fed `%s`/`%c` hole to bypass the Latin-1 Display funnels — the
  // format is split into segments around the raw `*_out` helper calls.
  bool argvBypassed = false;
  FailureOr<std::string> rustFormat = translatePrintfFormat(
      loc, call, literal, /*firstArgIndex=*/1, operands,
      /*allowArgvBypass=*/true, &argvBypassed);
  if (failed(rustFormat))
    return failure();

  // When a trailing bypass consumed the whole format tail, there is no
  // residual segment to print; emitting `print!("")` would be dead output.
  if (argvBypassed && rustFormat->empty() && operands.empty())
    return success();

  emitPrintMacro(loc, *rustFormat, operands);
  return success();
}

FailureOr<std::string> CImporter::translatePrintfFormat(
    Location loc, const clang::CallExpr *call,
    const clang::StringLiteral *literal, unsigned firstArgIndex,
    SmallVectorImpl<Value> &operands, bool allowArgvBypass,
    bool *argvBypassed) {
  // Translate the C format string into a Rust format string. The literal's
  // bytes already have C escapes decoded (a "\n" is a real newline byte);
  // the StringAttr printer re-escapes them for the textual assembly.
  llvm::StringRef format = literal->getString();
  std::string rustFormat;
  rustFormat.reserve(format.size());
  unsigned argIndex = firstArgIndex;
  // C99-43 C3: flushes the format accumulated so far as its own `print!`
  // call and starts a fresh segment, so a raw `*_out` helper call can be
  // sequenced in program order between two format segments. A no-op when the
  // pending segment is empty (avoids `print!("")`).
  auto flushSegment = [&]() {
    if (rustFormat.empty() && operands.empty())
      return;
    emitPrintMacro(loc, rustFormat, operands);
    rustFormat.clear();
    operands.clear();
  };
  for (size_t i = 0, n = format.size(); i < n; ++i) {
    char c = format[i];
    // C printf stops at an embedded NUL while Rust's print! would emit the
    // remaining bytes, and any byte outside printable ASCII (plus the
    // ordinary whitespace escapes) would reach the generated Rust source
    // verbatim and fail rustc's UTF-8 check; both are rejected rather than
    // silently diverging.
    if (c == '\0')
      return emitError(loc) << "unsupported: NUL byte in printf format";
    if ((c < 0x20 || c > 0x7e) && c != '\n' && c != '\t' && c != '\r')
      return emitError(loc)
             << "unsupported: non-printable or non-ASCII byte in printf "
                "format";
    if (c == '{') {
      rustFormat += "{{";
      continue;
    }
    if (c == '}') {
      rustFormat += "}}";
      continue;
    }
    if (c != '%') {
      rustFormat += c;
      continue;
    }
    if (++i >= n)
      return emitError(loc) << "unsupported: trailing '%' in printf format";
    if (format[i] == '%') {
      rustFormat += '%';
      continue;
    }
    // Parse `%[flags][width][.precision][length]conv` (C99 7.19.6.1). All
    // five C99 flags are recognized; width and precision are decimal
    // numbers ('*' forms consume a runtime argument and stay rejected);
    // lengths l/ll (64-bit) and h/hh (short/char range) are supported, L
    // is accepted on the floating conversions (long-double-as-f64, CTS
    // 00204), and j/z/t stay rejected.
    bool leftAlign = false;
    bool zeroPad = false;
    bool plusSign = false;
    bool spaceSign = false;
    bool altForm = false;
    while (i < n) {
      char flag = format[i];
      if (flag == '-')
        leftAlign = true;
      else if (flag == '0')
        zeroPad = true;
      else if (flag == '+')
        plusSign = true;
      else if (flag == ' ')
        spaceSign = true;
      else if (flag == '#')
        altForm = true;
      else
        break;
      ++i;
    }
    if (i < n && format[i] == '*')
      return emitError(loc)
             << "unsupported: '*' field width in printf format";
    std::string width;
    while (i < n && format[i] >= '0' && format[i] <= '9')
      width += format[i++];
    if (width.size() > 9)
      return emitError(loc) << "unsupported: printf field width too large";
    int precision = -1;
    if (i < n && format[i] == '.') {
      ++i;
      if (i < n && format[i] == '*')
        return emitError(loc)
               << "unsupported: '*' precision in printf format";
      std::string precisionDigits;
      while (i < n && format[i] >= '0' && format[i] <= '9')
        precisionDigits += format[i++];
      if (precisionDigits.size() > 9)
        return emitError(loc) << "unsupported: printf precision too large";
      // A '.' with no digits is precision zero (C99 7.19.6.1p4).
      precision = precisionDigits.empty() ? 0 : std::stoi(precisionDigits);
    }
    enum class Length { None, Long, LongLong, Short, Char, LongDouble, Size };
    Length lengthMod = Length::None;
    if (i < n && format[i] == 'l') {
      lengthMod = Length::Long;
      ++i;
      if (i < n && format[i] == 'l') {
        lengthMod = Length::LongLong;
        ++i;
      }
    } else if (i < n && format[i] == 'h') {
      lengthMod = Length::Short;
      ++i;
      if (i < n && format[i] == 'h') {
        lengthMod = Length::Char;
        ++i;
      }
    } else if (i < n && format[i] == 'L') {
      // The long double length modifier (CTS 00204): accepted on the
      // floating conversions, where the long-double-as-f64 policy makes
      // it behave exactly like the unmodified twin; rejected on the
      // integer conversions (undefined in C99 7.19.6.1p7) below.
      lengthMod = Length::LongDouble;
      ++i;
    } else if (i < n && (format[i] == 'j' || format[i] == 'z' ||
                         format[i] == 't')) {
      // size_t/intmax_t/ptrdiff_t (and their unsigned twins) are all 64-bit
      // on the LP64 x86-64 target the differential oracle uses, so `z`/`j`/`t`
      // behave exactly like `l`/`ll` on the integer conversions. Valid only
      // on the integer conversions; a float or c/s conversion is caught by
      // the length-validity checks below.
      lengthMod = Length::Size;
      ++i;
    }
    if (i >= n)
      return emitError(loc) << "unsupported: trailing '%' in printf format";
    char spec = format[i];
    std::string specName(1, spec);
    // The unknown-conversion diagnostic names the directive as spelled:
    // %La (the long-double hex-float form, whose output would render the
    // bits of the native 80-bit value) reports '%La', not '%a'.
    std::string directiveName =
        (lengthMod == Length::LongDouble ? "L" : "") + specName;
    // Validate the conversion before consuming an argument so an unknown
    // conversion is always the diagnostic, even when arguments are short.
    // %p stays rejected by design: pointer provenance is compiled away by
    // the pointer decomposition, so no address exists to print.
    bool isSignedConv = spec == 'd' || spec == 'i';
    bool isUnsignedConv =
        spec == 'u' || spec == 'x' || spec == 'X' || spec == 'o';
    bool isFloatConv = spec == 'f' || spec == 'F' || spec == 'e' ||
                       spec == 'E' || spec == 'g' || spec == 'G';
    if (!isSignedConv && !isUnsignedConv && !isFloatConv && spec != 'c' &&
        spec != 's')
      return emitError(loc) << "unsupported printf format specifier '%"
                            << directiveName << "'";
    // 'L' applies only to the floating conversions; on the integer ones
    // it is undefined in C99 and stays a located rejection (CTS 00204).
    if (lengthMod == Length::LongDouble && (isSignedConv || isUnsignedConv))
      return emitError(loc)
             << "unsupported: length modifier 'L' on printf '%" << specName
             << "'";
    // Flag and length validity (C99 7.19.6.1p6-7): '+'/' ' are defined
    // only for the signed and floating conversions, '#' only for x/X/o
    // and the floating conversions; both are undefined elsewhere and are
    // rejected rather than silently dropped. h/hh apply only to the
    // integer conversions; ll does not apply to the floating ones (l on a
    // floating conversion has no effect and is accepted, C99 7.19.6.1p7).
    if ((plusSign || spaceSign) && !isSignedConv && !isFloatConv)
      return emitError(loc) << "unsupported: '+' or ' ' flag on printf '%"
                            << specName << "'";
    if (altForm && !isFloatConv && spec != 'x' && spec != 'X' && spec != 'o')
      return emitError(loc)
             << "unsupported: '#' flag on printf '%" << specName << "'";
    if ((lengthMod == Length::Short || lengthMod == Length::Char) &&
        !isSignedConv && !isUnsignedConv)
      return emitError(loc)
             << "unsupported: length modifier 'h' on printf '%" << specName
             << "'";
    if (lengthMod == Length::LongLong && isFloatConv)
      return emitError(loc)
             << "unsupported: length modifier 'll' on printf '%" << specName
             << "'";
    // z/j/t are integer-conversion lengths; on a floating conversion they are
    // undefined (C99 7.19.6.1p7) and stay a located rejection.
    if (lengthMod == Length::Size && isFloatConv)
      return emitError(loc)
             << "unsupported: length modifier 'z'/'j'/'t' on printf '%"
             << specName << "'";
    if (lengthMod != Length::None && (spec == 'c' || spec == 's'))
      return emitError(loc) << "unsupported: length modifier on printf '%"
                            << specName << "'";
    if (zeroPad && (spec == 'c' || spec == 's'))
      return emitError(loc)
             << "unsupported: '0' flag on printf '%" << specName << "'";
    if (precision >= 0 && spec == 'c')
      return emitError(loc) << "unsupported: precision on printf '%c'";
    bool isLong = lengthMod == Length::Long ||
                  lengthMod == Length::LongLong || lengthMod == Length::Size;
    // Renders the Rust format placeholder for a numeric directive: the C
    // width maps 1:1 ("%5d" -> "{:5}"), '-' to left alignment ("%-5d" ->
    // "{:<5}"), '0' to Rust's sign-aware zero pad ("%05d" -> "{:05}"),
    // and x/X/o append their radix marker ("%04X" -> "{:04X}"). A flag
    // without a width is a no-op in C and is dropped. C ignores '0' when
    // '-' is present, so left alignment wins.
    auto placeholderFor = [&](llvm::StringRef radix) {
      if (width.empty() && radix.empty())
        return std::string("{}");
      std::string text = "{:";
      if (!width.empty()) {
        if (leftAlign)
          text += '<';
        else if (zeroPad)
          text += '0';
        text += width;
      }
      text += radix.str();
      text += '}';
      return text;
    };
    // Renders the placeholder for a %c/%s directive with a width: C
    // right-aligns text to the field by default where Rust's string
    // formatting left-aligns, so the alignment is always explicit.
    auto textPlaceholder = [&]() {
      if (width.empty())
        return std::string("{}");
      std::string text = "{:";
      text += leftAlign ? '<' : '>';
      text += width;
      text += '}';
      return text;
    };
    // The C99-flag bitmask shared by the `__emitrust_fmt_*` helpers
    // (kept in sync with the emitted helper sources): '-'=1, '0'=2,
    // '+'=4, ' '=8, '#'=16, uppercase conversion=32.
    int flagsMask = (leftAlign ? 1 : 0) | (zeroPad ? 2 : 0) |
                    (plusSign ? 4 : 0) | (spaceSign ? 8 : 0) |
                    (altForm ? 16 : 0);
    int widthValue = width.empty() ? 0 : std::stoi(width);
    auto i32Type = builder.getIntegerType(32);
    auto stringType =
        emitrust::OpaqueType::get(builder.getContext(), "String");
    if (argIndex >= call->getNumArgs())
      return emitError(loc) << "unsupported: too few arguments to printf";
    const clang::Expr *argExpr = call->getArg(argIndex);
    unsigned argNumber = argIndex++;

    // C99-43 C3: an argv-fed `%s`/`%c` hole in the stdout `print!` context
    // bypasses the Latin-1 `__emitrust_cstr`/`__emitrust_fmt_c` Display
    // funnels (whose byte-to-char widening double-encodes non-ASCII argument
    // bytes) by flushing the pending format segment and writing the raw bytes
    // through the on-demand `*_out` helpers on the same buffered stdout handle.
    if (allowArgvBypass && mainArgvTableValue) {
      if (spec == 's') {
        if (const clang::Expr *idxExpr = matchArgvWholeSubscript(argExpr)) {
          if (argvBypassed)
            *argvBypassed = true;
          flushSegment();
          FailureOr<Value> slice = emitArgvArgSlice(loc, idxExpr);
          if (failed(slice))
            return failure();
          if (precision >= 0) {
            // `%.Ns`: at most N raw bytes, stopping earlier at a NUL.
            needsCStrNOutHelper = true;
            Value count = createIntConstant(loc, builder.getIntegerType(64),
                                            static_cast<int64_t>(precision));
            builder.create<emitrust::CallOpaqueOp>(
                loc, TypeRange(),
                builder.getStringAttr("__emitrust_cstr_n_out"),
                /*args=*/ArrayAttr(), ValueRange{*slice, count});
          } else {
            needsCStrOutHelper = true;
            builder.create<emitrust::CallOpaqueOp>(
                loc, TypeRange(),
                builder.getStringAttr("__emitrust_cstr_out"),
                /*args=*/ArrayAttr(), ValueRange{*slice});
          }
          continue;
        }
      } else if (spec == 'c') {
        if (const clang::ArraySubscriptExpr *byte = matchArgvByteRead(argExpr)) {
          if (argvBypassed)
            *argvBypassed = true;
          flushSegment();
          FailureOr<Value> place = emitArgvByteLValue(byte, loc);
          if (failed(place))
            return failure();
          Value byteValue = loadPlace(loc, *place);
          needsByteOutHelper = true;
          builder.create<emitrust::CallOpaqueOp>(
              loc, TypeRange(),
              builder.getStringAttr("__emitrust_byte_out"),
              /*args=*/ArrayAttr(), ValueRange{byteValue});
          continue;
        }
      }
    }

    if (spec == 's') {
      std::optional<unsigned> stringPrecision;
      if (precision >= 0)
        stringPrecision = static_cast<unsigned>(precision);
      FailureOr<Value> text = emitPrintfStringArg(argExpr, stringPrecision);
      if (failed(text))
        return failure();
      operands.push_back(*text);
      rustFormat += textPlaceholder();
      continue;
    }

    FailureOr<Value> argument = emitRValue(argExpr);
    if (failed(argument))
      return failure();
    Type argType = (*argument).getType();

    if (isFloatConv) {
      if (!llvm::isa<Float64Type>(argType))
        return emitError(loc) << "unsupported: printf argument " << argNumber
                              << " does not match its format specifier";
      bool plainF = spec == 'f' && precision < 0 && width.empty() &&
                    !leftAlign && !zeroPad && !plusSign && !spaceSign &&
                    !altForm;
      if (plainF) {
        // C's %f prints six decimals; Rust's {:.6} matches it for every
        // finite value and for infinities, but spells NaN as "NaN" where
        // C prints "nan"/"-nan". The argument is therefore routed through
        // the module-level `__emitrust_fmt_f64` helper (emitted once, on
        // demand) and printed with a plain `{}`. (`%lf` is identical to
        // `%f` in C99.)
        needsFloatFormatHelper = true;
        *argument = builder
                        .create<emitrust::CallOpaqueOp>(
                            loc, TypeRange{stringType},
                            builder.getStringAttr("__emitrust_fmt_f64"),
                            /*args=*/ArrayAttr(), ValueRange{*argument})
                        .getResult(0);
        operands.push_back(*argument);
        rustFormat += "{}";
        continue;
      }
      // Every other floating directive goes through the module-level
      // `__emitrust_fmt_float` helper, which implements the C99 f/e/g
      // algorithms (including the glibc %#g rounding-carry quirk) over
      // Rust's exact correctly-rounded decimal conversion; the directive's
      // compile-time parameters travel as i32 constants.
      int convCode = (spec == 'e' || spec == 'E') ? 1
                     : (spec == 'g' || spec == 'G') ? 2
                                                    : 0;
      if (spec == 'F' || spec == 'E' || spec == 'G')
        flagsMask |= 32;
      needsFloatFormatExtHelper = true;
      Value convValue = createIntConstant(loc, i32Type, convCode);
      Value precisionValue = createIntConstant(loc, i32Type, precision);
      Value widthConst = createIntConstant(loc, i32Type, widthValue);
      Value flagsValue = createIntConstant(loc, i32Type, flagsMask);
      Value formatted =
          builder
              .create<emitrust::CallOpaqueOp>(
                  loc, TypeRange{stringType},
                  builder.getStringAttr("__emitrust_fmt_float"),
                  /*args=*/ArrayAttr(),
                  ValueRange{*argument, convValue, precisionValue,
                             widthConst, flagsValue})
              .getResult(0);
      operands.push_back(formatted);
      rustFormat += "{}";
      continue;
    }

    auto argIntType = llvm::dyn_cast<IntegerType>(argType);
    bool isIntArgument = argIntType && argIntType.getWidth() > 1;

    if (spec == 'c') {
      // C converts the argument to unsigned char and prints that byte;
      // the i32 argument (chars arrive int-promoted) goes through the
      // `__emitrust_fmt_c` helper (ASCII-only, see design.md C99-48).
      if (!isIntArgument)
        return emitError(loc) << "unsupported: printf argument " << argNumber
                              << " does not match its format specifier";
      operands.push_back(wrapCharFormat(loc, *argument));
      rustFormat += textPlaceholder();
      continue;
    }

    // Integer conversions. d/i print signed; u/x/X/o print the value as
    // unsigned, so the argument is `as`-cast to the unsigned type of the
    // directive's width — a negative signed argument then prints its
    // two's-complement bit pattern ("%x" of -1 is ffffffff), exactly like
    // C. An argument of a different width is `as`-cast as well, which
    // truncates to the low bits just like C's varargs read on x86-64
    // (printf("%d", sizeof(x)) prints the low 32 bits of the size_t); the
    // h/hh lengths reuse the same cast to reduce the int-promoted
    // argument to short/char range (C99 7.19.6.1p7).
    llvm::StringRef radix;
    switch (spec) {
    case 'x':
      radix = "x";
      break;
    case 'X':
      radix = "X";
      break;
    case 'o':
      radix = "o";
      break;
    default:
      break;
    }
    if (!isIntArgument)
      return emitError(loc) << "unsupported: printf argument " << argNumber
                            << " does not match its format specifier";
    unsigned bits = isLong                      ? 64
                    : lengthMod == Length::Short ? 16
                    : lengthMod == Length::Char  ? 8
                                                 : 32;
    IntegerType target =
        isSignedConv
            ? builder.getIntegerType(bits)
            : IntegerType::get(builder.getContext(), bits,
                               IntegerType::Unsigned);
    Value narrowed = castToIntType(loc, *argument, target);
    if (precision < 0 && !plusSign && !spaceSign && !altForm) {
      // Flags/width-only directives map 1:1 onto Rust format specs.
      operands.push_back(narrowed);
      rustFormat += placeholderFor(radix);
      continue;
    }
    // Precision or the '+'/' '/'#' flags have no Rust format equivalent
    // with C semantics ('0' is ignored next to a precision, the sign and
    // 0x/0 prefixes sit inside the zero padding, ...); the directive goes
    // through the module-level `__emitrust_fmt_i64`/`__emitrust_fmt_u64`
    // helpers, which implement the C99 rules exactly over the value
    // widened to 64 bits (sign- or zero-extended per the conversion).
    if (spec == 'X')
      flagsMask |= 32;
    Value widened = castToIntType(
        loc, narrowed,
        isSignedConv ? builder.getIntegerType(64)
                     : IntegerType::get(builder.getContext(), 64,
                                        IntegerType::Unsigned));
    Value precisionValue = createIntConstant(loc, i32Type, precision);
    Value widthConst = createIntConstant(loc, i32Type, widthValue);
    Value flagsValue = createIntConstant(loc, i32Type, flagsMask);
    SmallVector<Value> helperArgs{widened};
    llvm::StringRef helperName = "__emitrust_fmt_i64";
    if (!isSignedConv) {
      helperName = "__emitrust_fmt_u64";
      int base = spec == 'o' ? 8 : spec == 'u' ? 10 : 16;
      helperArgs.push_back(createIntConstant(loc, i32Type, base));
      needsIntFormatUnsignedHelper = true;
    } else {
      needsIntFormatSignedHelper = true;
    }
    helperArgs.push_back(precisionValue);
    helperArgs.push_back(widthConst);
    helperArgs.push_back(flagsValue);
    Value formatted = builder
                          .create<emitrust::CallOpaqueOp>(
                              loc, TypeRange{stringType},
                              builder.getStringAttr(helperName),
                              /*args=*/ArrayAttr(), helperArgs)
                          .getResult(0);
    operands.push_back(formatted);
    rustFormat += "{}";
  }
  if (argIndex != call->getNumArgs())
    return emitError(loc) << "unsupported: too many arguments to printf";
  return rustFormat;
}

FailureOr<Value> CImporter::emitSprintf(const clang::CallExpr *call,
                                        bool isSnprintf) {
  Location loc = translateLoc(call->getBeginLoc());
  // `snprintf(dest, size, fmt, ...)` carries a size bound at argument 1 that
  // shifts the format literal and the variadic arguments one position past
  // `sprintf(dest, fmt, ...)`.
  const char *name = isSnprintf ? "snprintf" : "sprintf";
  unsigned formatArgIndex = isSnprintf ? 2 : 1;
  unsigned minArgs = isSnprintf ? 3 : 2;
  if (call->getNumArgs() < minArgs)
    return emitError(loc) << "unsupported: " << name
                          << " requires a destination and a format string";
  const clang::Expr *formatExpr =
      call->getArg(formatArgIndex)->IgnoreParenImpCasts();
  const auto *literal = llvm::dyn_cast<clang::StringLiteral>(formatExpr);
  if (!literal || !literal->isOrdinary())
    return emitError(loc) << "unsupported: " << name
                          << " format must be an ordinary string literal";
  // snprintf's size bound (argument 1), widened to i64 for the helper. It is
  // materialized BEFORE the destination's mutable borrow, so no load
  // intervenes between that borrow and the helper call.
  Value sizeValue;
  if (isSnprintf) {
    FailureOr<Value> size = emitRValue(call->getArg(1));
    if (failed(size))
      return failure();
    sizeValue = castToIntType(loc, *size, builder.getIntegerType(64));
  }
  FailureOr<PtrExprValue> dst = emitCharRegionArg(call->getArg(0));
  if (failed(dst))
    return failure();

  // The format arguments materialize first (through the printf-shared
  // directive grammar) and collapse into a String, so no load intervenes
  // between the mutable destination borrow below and the helper call
  // consuming it.
  SmallVector<Value> operands;
  FailureOr<std::string> rustFormat = translatePrintfFormat(
      loc, call, literal, /*firstArgIndex=*/formatArgIndex + 1, operands);
  if (failed(rustFormat))
    return failure();
  SmallVector<Attribute> callArguments;
  callArguments.push_back(builder.getStringAttr(*rustFormat));
  for (unsigned i = 0, e = operands.size(); i < e; ++i)
    callArguments.push_back(builder.getIndexAttr(i));
  auto stringType = emitrust::OpaqueType::get(builder.getContext(), "String");
  Value text = builder
                   .create<emitrust::CallOpaqueOp>(
                       loc, TypeRange{stringType},
                       builder.getStringAttr("format!"),
                       builder.getArrayAttr(callArguments), operands)
                   .getResult(0);

  // The helper's pinned `s: &str` parameter is fed a `&String` borrow
  // (deref coercion applies at the argument position): the String value
  // has no place of its own, so it is staged through a String variable
  // whose shared borrow is taken before the destination's mutable borrow
  // below (distinct objects, so the borrows coexist).
  Value stringPlace =
      builder
          .create<emitrust::VariableOp>(loc,
                                        emitrust::LValueType::get(stringType))
          .getResult();
  builder.create<emitrust::AssignOp>(loc, stringPlace, text);
  Value textRef = builder
                      .create<emitrust::AddrOfOp>(
                          loc, emitrust::RefType::get(stringType), stringPlace,
                          /*isMut=*/false)
                      .getResult();

  // The destination borrows mutably from its cursor, exactly like the
  // <string.h> copy helpers (a string-literal region rejects here).
  FailureOr<Value> dstSlice = emitCharRegionSlice(loc, *dst, /*isMut=*/true);
  if (failed(dstSlice))
    return failure();
  if (isSnprintf) {
    needsSnprintfHelper = true;
    return builder
        .create<emitrust::CallOpaqueOp>(
            loc, TypeRange{builder.getI32Type()},
            builder.getStringAttr("__emitrust_snprintf"),
            /*args=*/ArrayAttr(), ValueRange{*dstSlice, sizeValue, textRef})
        .getResult(0);
  }
  needsSprintfHelper = true;
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{builder.getI32Type()},
          builder.getStringAttr("__emitrust_sprintf"),
          /*args=*/ArrayAttr(), ValueRange{*dstSlice, textRef})
      .getResult(0);
}

FailureOr<Value> CImporter::emitRustStrLiteral(Location loc,
                                               llvm::StringRef data,
                                               llvm::StringRef context) {
  // The literal's decoded bytes become a Rust string literal emitted
  // verbatim into the generated source: an embedded NUL would diverge
  // from C (which stops printing there) and a non-ASCII byte would fail
  // rustc's UTF-8 check, so both are rejected; quote, backslash, and the
  // whitespace escapes are re-escaped for the Rust spelling.
  std::string text = "\"";
  for (char c : data) {
    if (c == '\0')
      return emitError(loc) << "unsupported: NUL byte in " << context;
    if ((c < 0x20 || c > 0x7e) && c != '\n' && c != '\t' && c != '\r')
      return emitError(loc)
             << "unsupported: non-printable or non-ASCII byte in " << context;
    switch (c) {
    case '\n':
      text += "\\n";
      break;
    case '\t':
      text += "\\t";
      break;
    case '\r':
      text += "\\r";
      break;
    case '"':
      text += "\\\"";
      break;
    case '\\':
      text += "\\\\";
      break;
    default:
      text += c;
    }
  }
  text += '"';
  auto strType = emitrust::OpaqueType::get(builder.getContext(), "&'static str");
  return builder
      .create<emitrust::LiteralOp>(loc, strType, builder.getStringAttr(text))
      .getResult();
}

/// W2.3: matches a `.c_str()` call on a recognized `std::string` object —
/// `expr` after `IgnoreParenImpCasts` is a `CXXMemberCallExpr` naming a
/// method `c_str` whose parent record is in namespace `std` — and returns
/// the underlying implicit-object expression. Returns null for every other
/// shape (the caller falls through to the ordinary printf '%s' shapes).
static const clang::Expr *matchStlCStrCall(const clang::Expr *expr) {
  const auto *call = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr);
  if (!call)
    return nullptr;
  const clang::CXXMethodDecl *method = call->getMethodDecl();
  if (!method || method->getDeclName().getAsString() != "c_str" ||
      !method->getParent()->isInStdNamespace())
    return nullptr;
  return call->getImplicitObjectArgument();
}

FailureOr<Value>
CImporter::emitPrintfStringArg(const clang::Expr *expr,
                               std::optional<unsigned> precision) {
  // The array-to-pointer decay wrapping both supported shapes is implicit;
  // strip it (and parentheses) to see the underlying literal or lvalue.
  // A `__func__`-family predefined identifier prints its function-name
  // literal through the same literal path (C99-29); the check runs before
  // the char-array branch below, which would otherwise claim the
  // predefined identifier's `const char[N]` lvalue type.
  const clang::Expr *arg = expr->IgnoreParenImpCasts();
  Location loc = translateLoc(arg->getBeginLoc());
  // FR-64: a lifted constant-fill string local prints its `String` by
  // `Display` — load the value directly (rendered `a`, auto-borrowed by the
  // format macro), bypassing the i8-slice `__emitrust_cstr` `%s` path. A
  // precision on such an argument is not modeled (the buffer has no NUL-free
  // suffix contract), so it keeps the historical rejection below.
  if (!precision)
    if (const clang::VarDecl *local = asLoadedLocalVarRef(arg))
      if (stringFillLocals.contains(local)) {
        auto stringType =
            emitrust::OpaqueType::get(builder.getContext(), "String");
        return builder
            .create<emitrust::LoadOp>(loc, stringType, symbols[local])
            .getResult();
      }
  // W2.3: `printf("%s", s.c_str())` — the idiomatic C++ shape, since
  // `std::string` has no implicit conversion to `const char*` — borrows
  // the String place shared, exactly like `__emitrust_sprintf`'s staged
  // String borrow; deref coercion `&String` -> `&str` applies at the
  // format-argument position. This is the ONLY position `.c_str()` is
  // recognized in (design.md's STL OUT list covers every other use).
  if (const clang::Expr *receiverExpr = matchStlCStrCall(arg)) {
    if (precision)
      return emitError(loc)
             << "unsupported: a precision on a '%s' argument fed by "
                "std::string::c_str()";
    FailureOr<Value> receiver =
        emitLValue(receiverExpr->IgnoreParenImpCasts());
    if (failed(receiver))
      return failure();
    auto lvalueType = llvm::dyn_cast<emitrust::LValueType>((*receiver).getType());
    if (!lvalueType || !isStlOpaqueType(lvalueType.getValueType()) ||
        llvm::cast<emitrust::OpaqueType>(lvalueType.getValueType()).getValue() !=
            "String")
      return emitError(loc)
             << "unsupported: c_str() receiver is not a recognized "
                "std::string";
    return builder
        .create<emitrust::AddrOfOp>(
            loc, emitrust::RefType::get(lvalueType.getValueType()), *receiver,
            /*is_mut=*/false)
        .getResult();
  }
  if (const clang::StringLiteral *literal = underlyingStringLiteral(arg)) {
    if (!literal->isOrdinary())
      return emitError(loc)
             << "unsupported: non-ordinary string literal in printf '%s'";
    // A %.Ns precision truncates at import time: C never reads past the
    // Nth byte, so only the retained prefix is validated/escaped below.
    llvm::StringRef data = literal->getString();
    if (precision && *precision < data.size())
      data = data.take_front(*precision);
    return emitRustStrLiteral(loc, data, "printf '%s' string literal");
  }
  // Renders a borrowed i8 slice through the on-demand `__emitrust_cstr`
  // helper (stops at the first NUL, like C's %s) or, under a %.Ns
  // precision, through `__emitrust_cstr_n` (stops at N bytes or the first
  // NUL, whichever comes first; C99 7.19.6.1p8 allows the array to lack a
  // terminator when the precision bounds the read).
  auto wrapCStr = [&](Location loc, Value slice) -> Value {
    auto stringType =
        emitrust::OpaqueType::get(builder.getContext(), "String");
    if (precision) {
      needsCStrNHelper = true;
      Value count = createIntConstant(loc, builder.getIntegerType(64),
                                      static_cast<int64_t>(*precision));
      return builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{stringType},
              builder.getStringAttr("__emitrust_cstr_n"),
              /*args=*/ArrayAttr(), ValueRange{slice, count})
          .getResult(0);
    }
    needsCStrHelper = true;
    return builder
        .create<emitrust::CallOpaqueOp>(loc, TypeRange{stringType},
                                        builder.getStringAttr("__emitrust_cstr"),
                                        /*args=*/ArrayAttr(),
                                        ValueRange{slice})
        .getResult(0);
  };
  // A char-array lvalue is borrowed whole (`emitrust.slice_of` at index 0)
  // and rendered by the `__emitrust_cstr` helper, which — like C's %s —
  // stops at the first NUL.
  if (astContext().getAsConstantArrayType(arg->getType()) &&
      arg->isLValue()) {
    FailureOr<Value> place = emitLValue(arg);
    if (failed(place))
      return failure();
    auto lvalueType = llvm::cast<emitrust::LValueType>((*place).getType());
    auto arrayType =
        llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType());
    if (!arrayType || arrayType.getElementType() != builder.getIntegerType(8))
      return emitError(loc)
             << "unsupported: printf '%s' argument must be a string literal "
                "or a char array";
    Value zero = createIntConstant(loc, builder.getIntegerType(64), 0);
    auto sliceRefType = emitrust::RefType::get(
        emitrust::SliceType::get(arrayType.getElementType()));
    Value slice = builder
                      .create<emitrust::SliceOfOp>(loc, sliceRefType, *place,
                                                   zero, /*is_mut=*/false)
                      .getResult();
    return wrapCStr(loc, slice);
  }
  // A strchr/strrchr result prints the searched region's byte run from
  // the found index: the helper's i64 index (relative to the argument's
  // cursor) offsets the cursor, and the region is re-sliced there for
  // `__emitrust_cstr`. A not-found result is C's NULL, whose %s print is
  // undefined in C; the -1 index makes the slice borrow panic instead of
  // reading out of bounds.
  bool reverse = false;
  if (const clang::CallExpr *search = asHostedStrchrCall(arg, reverse)) {
    PtrExprValue region;
    FailureOr<Value> index = emitStrchrIndex(search, reverse, region);
    if (failed(index))
      return failure();
    Value found =
        builder.create<arith::AddIOp>(loc, region.cursor, *index)
            .getResult();
    PtrExprValue at{region.base, found, region.literalBacking};
    FailureOr<Value> slice = emitCharRegionSlice(loc, at, /*isMut=*/false);
    if (failed(slice))
      return failure();
    return wrapCStr(loc, *slice);
  }
  // `&arr[i]` (or `&p[i]` over a decomposed pointer) prints the region's
  // byte run from element i, through the same slice + `__emitrust_cstr`
  // lowering as the whole-array shape (CTS-L1; 00180.c prints &a[1]).
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(arg))
    if (unary->getOpcode() == clang::UO_AddrOf &&
        llvm::isa<clang::ArraySubscriptExpr>(
            stripTrivia(unary->getSubExpr()))) {
      FailureOr<PtrExprValue> pointer = emitCharRegionArg(arg);
      if (failed(pointer))
        return failure();
      FailureOr<Value> slice =
          emitCharRegionSlice(loc, *pointer, /*isMut=*/false);
      if (failed(slice))
        return failure();
      return wrapCStr(loc, *slice);
    }
  // A decomposed `char *` prints the backing byte run from its cursor:
  // `emitrust.slice_of` of the region place at the cursor, rendered by
  // the same `__emitrust_cstr` helper as char arrays (both stop at the
  // first NUL, like C's %s). Two region shapes qualify: a pointer into a
  // string-literal region (its read-only backing array, CTS-P1) and the
  // FR-28 slice-parameter class (a slice-classified `char *` parameter,
  // whose base place is the deref'd `!emitrust.lvalue<!emitrust.slice<i8>>`,
  // CTS-L2). The decomposed-pointer gate keeps pointer-shaped arguments
  // without a decomposed pointer (casts of scalar addresses, ...) on the
  // generic rejection below.
  if (isPointerType(expr->getType()) && involvesDecomposedPointer(expr) &&
      isDecomposedPointerExpr(expr)) {
    FailureOr<PtrExprValue> pointer = emitPointerRValue(expr);
    if (failed(pointer))
      return failure();
    Value backingPlace = pointer->literalBacking;
    Type elementType;
    if (backingPlace) {
      auto lvalueType =
          llvm::cast<emitrust::LValueType>(backingPlace.getType());
      elementType = llvm::cast<emitrust::ArrayType>(lvalueType.getValueType())
                        .getElementType();
    } else if (pointer->base) {
      auto it = symbols.find(pointer->base);
      if (it != symbols.end()) {
        auto lvalueType =
            llvm::dyn_cast<emitrust::LValueType>(it->second.getType());
        auto sliceType =
            lvalueType ? llvm::dyn_cast<emitrust::SliceType>(
                             lvalueType.getValueType())
                       : emitrust::SliceType();
        if (sliceType &&
            sliceType.getElementType() == builder.getIntegerType(8)) {
          backingPlace = it->second;
          elementType = sliceType.getElementType();
        }
      } else if (!pointer->base->hasLocalStorage()) {
        // A pointer into a global char array (the 00217 shape) prints the
        // staged copy's byte run: the copy is taken fresh at the print,
        // so every earlier write — element, wide-byte, or cell — is
        // visible in it (CTS-P11).
        FailureOr<std::pair<Value, std::string>> staged =
            stageGlobalCopy(loc, pointer->base);
        if (failed(staged))
          return failure();
        auto lvalueType =
            llvm::cast<emitrust::LValueType>(staged->first.getType());
        auto arrayType =
            llvm::dyn_cast<emitrust::ArrayType>(lvalueType.getValueType());
        if (arrayType &&
            arrayType.getElementType() == builder.getIntegerType(8)) {
          backingPlace = staged->first;
          elementType = arrayType.getElementType();
        }
      }
    }
    if (!backingPlace)
      return emitError(loc)
             << "unsupported: printf '%s' argument must be a string literal, "
                "a char array, a char slice parameter, or a pointer into a "
                "string literal";
    Value cursor =
        pointer->cursor
            ? pointer->cursor
            : createIntConstant(loc, builder.getIntegerType(64), 0);
    auto sliceRefType =
        emitrust::RefType::get(emitrust::SliceType::get(elementType));
    Value slice = builder
                      .create<emitrust::SliceOfOp>(loc, sliceRefType,
                                                   backingPlace, cursor,
                                                   /*is_mut=*/false)
                      .getResult();
    return wrapCStr(loc, slice);
  }
  return emitError(loc) << "unsupported: printf '%s' argument must be a "
                           "string literal or a char array";
}

Value CImporter::wrapCharFormat(Location loc, Value value) {
  needsCharFormatHelper = true;
  Value promoted = castToIntType(loc, value, builder.getI32Type());
  auto charType = emitrust::OpaqueType::get(builder.getContext(), "char");
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{charType}, builder.getStringAttr("__emitrust_fmt_c"),
          /*args=*/ArrayAttr(), ValueRange{promoted})
      .getResult(0);
}

LogicalResult CImporter::emitPuts(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc) << "unsupported: puts requires exactly one argument";
  // C's puts writes the string then a newline; println! of the %s-shaped
  // value matches byte-for-byte (both supported shapes reject the bytes
  // Rust could not reproduce).
  FailureOr<Value> text = emitPrintfStringArg(call->getArg(0));
  if (failed(text))
    return failure();
  builder.create<emitrust::CallOpaqueOp>(
      loc, TypeRange(), builder.getStringAttr("println!"),
      builder.getArrayAttr(
          {builder.getStringAttr("{}"), builder.getIndexAttr(0)}),
      ValueRange{*text});
  return success();
}

FailureOr<Value> CImporter::emitStrlenCall(const clang::CallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  if (call->getNumArgs() != 1)
    return emitError(loc)
           << "unsupported: strlen requires exactly one argument";
  FailureOr<PtrExprValue> pointer = emitCharRegionArg(call->getArg(0));
  if (failed(pointer))
    return failure();
  FailureOr<Value> slice =
      emitCharRegionSlice(loc, *pointer, /*isMut=*/false);
  if (failed(slice))
    return failure();
  needsStrlenHelper = true;
  Value count =
      builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{builder.getIntegerType(64)},
              builder.getStringAttr("__emitrust_strlen"),
              /*args=*/ArrayAttr(), ValueRange{*slice})
          .getResult(0);
  // Convert the i64 count to the call's declared result type (`int` in the
  // K&R-style `int strlen(char *)` prototype, size_t otherwise), matching
  // C's conversion of the returned value.
  FailureOr<Type> resultType = mapType(call->getType(), loc);
  if (failed(resultType))
    return failure();
  auto intType = llvm::dyn_cast<IntegerType>(*resultType);
  if (!intType)
    return emitError(loc) << "unsupported: strlen result type";
  return castToIntType(loc, count, intType);
}

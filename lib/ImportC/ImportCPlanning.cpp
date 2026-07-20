//===- ImportCPlanning.cpp - Pass-A per-TU AST planners ---------*- C++ -*-===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// CImporter's Pass-A planners: pure-AST, per-translation-unit analyses run
/// BEFORE any IR is built for that TU (`planOwners`, `planCellSlices`,
/// `planFnPtrAliases`, `planCursorParams`, `planVaMonomorph`,
/// `collectDeclTypeRecords`, `planFnPtrMembers`), plus the shared
/// interprocedural union-find/call-edge scaffolding they use
/// (`VarDeclUnionFind`, `forEachDataPointerCallArg`,
/// `collectPassAFunctionDefinitions`, `CellSliceBodyScan`,
/// `asDecayedGlobalArrayArg`, `asPointerParamRead`, `collectAddressTaken`).
/// Every one of these passes runs per TU over fresh, `Decl*`-keyed state —
/// none of them merge across translation units (see `WholeProgramInfo` in
/// CImporterInternal.h and `CImporter::collectWholeProgramInfo` in
/// ImportC.cpp for the whole-program, symbol-name-keyed substrate a project
/// import ALSO builds, kept separate from this file because these planners
/// stay genuinely per-TU passes).
///
/// Split out of ImportC.cpp by pure code motion (W3.2 COMMIT A closing
/// sub-step); see CImporterInternal.h for the CImporter class declaration
/// this file implements.
//
//===----------------------------------------------------------------------===//

#include "CImporterInternal.h"

using namespace mlir;

/// Collects every call expression below `stmt`, in source order.
static void collectCallExprs(const clang::Stmt *stmt,
                             SmallVectorImpl<const clang::CallExpr *> &calls) {
  if (!stmt)
    return;
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt))
    calls.push_back(call);
  for (const clang::Stmt *child : stmt->children())
    collectCallExprs(child, calls);
}

namespace {
/// Union-find over storage/parameter declarations, the shared
/// interprocedural machinery of the Pass-A planners (`planOwners`,
/// `planCellSlices`): nodes register on first touch, `find` compresses
/// paths, and `unite` links roots. Union-find transitively closes as
/// edges are added, so one walk over every body reaches the fixpoint.
/// `nodes` snapshots the touched set for an aggregation that keeps
/// calling `find` (which compresses the underlying map).
class VarDeclUnionFind {
public:
  /// Returns `decl`'s class root, registering an unseen node as its own
  /// root and compressing the path walked.
  const clang::VarDecl *find(const clang::VarDecl *decl) {
    parent.try_emplace(decl, decl);
    const clang::VarDecl *root = decl;
    while (parent[root] != root)
      root = parent[root];
    while (parent[decl] != root) {
      const clang::VarDecl *next = parent[decl];
      parent[decl] = root;
      decl = next;
    }
    return root;
  }

  /// Unites `b`'s class into `a`'s.
  void unite(const clang::VarDecl *a, const clang::VarDecl *b) {
    const clang::VarDecl *rootB = find(b);
    const clang::VarDecl *rootA = find(a);
    parent[rootB] = rootA;
  }

  /// Snapshots every node ever touched.
  SmallVector<const clang::VarDecl *> nodes() const {
    SmallVector<const clang::VarDecl *> result;
    result.reserve(parent.size());
    for (const auto &entry : parent)
      result.push_back(entry.first);
    return result;
  }

private:
  llvm::DenseMap<const clang::VarDecl *, const clang::VarDecl *> parent;
};
} // namespace

/// Walks every call in `body` whose callee resolves to a non-variadic
/// definition of matching arity and invokes `visit` once per data-pointer
/// (non-function-pointer) callee parameter with the argument bound to it —
/// the shared call-edge scaffold of the Pass-A planners. Callees without
/// a definition in this TU add no edge: passing a region to them stays on
/// the Phase-1b call lowering.
static void forEachDataPointerCallArg(
    const clang::Stmt *body,
    llvm::function_ref<void(const clang::ParmVarDecl *, const clang::Expr *)>
        visit) {
  SmallVector<const clang::CallExpr *> calls;
  collectCallExprs(body, calls);
  for (const clang::CallExpr *call : calls) {
    const clang::FunctionDecl *callee = call->getDirectCallee();
    if (!callee || callee->isVariadic())
      continue;
    const clang::FunctionDecl *definition = callee->getDefinition();
    if (!definition || !definition->hasBody() ||
        call->getNumArgs() != definition->getNumParams())
      continue;
    for (auto [index, argument] : llvm::enumerate(call->arguments())) {
      const clang::ParmVarDecl *param = definition->getParamDecl(index);
      if (!isPointerType(param->getType()) ||
          isFunctionPointer(param->getType()))
        continue;
      visit(param, argument);
    }
  }
}

SmallVector<const clang::FunctionDecl *>
CImporter::collectPassAFunctionDefinitions(
    const clang::TranslationUnitDecl *unit) const {
  SmallVector<const clang::FunctionDecl *> definitions;
  for (const clang::Decl *decl : unit->decls())
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl))
      if (func->doesThisDeclarationHaveABody() && !func->isVariadic() &&
          !isSystemHeaderDecl(func))
        definitions.push_back(func);
  return definitions;
}

void CImporter::planOwners(const clang::TranslationUnitDecl *unit,
                           bool soleTranslationUnit) {
  // Interprocedural union-find over storage bases (local arrays and
  // scalars) and the data-pointer parameters of function definitions.
  VarDeclUnionFind unionFind;
  // Declarations whose class must not be promoted: unresolvable pointer
  // arguments, pointer-to-pointer parameters, invalidated (escaping)
  // regions. Membership is checked per node during aggregation, so a
  // poison mark survives later unions.
  llvm::SmallPtrSet<const clang::VarDecl *, 8> poisoned;

  for (const clang::FunctionDecl *func :
       collectPassAFunctionDefinitions(unit)) {
    PointerRegionAnalysis analysis;
    analysis.literalTemps = &literalTemps;
    analysis.analyze(astContext(), func->getBody());

    // Data-pointer parameters are class nodes; a pointer-to-pointer
    // parameter poisons its class (it has no i64-index representation).
    for (const clang::ParmVarDecl *param : func->parameters()) {
      if (!isPointerType(param->getType()) ||
          isFunctionPointer(param->getType()))
        continue;
      (void)unionFind.find(param);
      if (param->getType()
              .getCanonicalType()
              ->getPointeeType()
              .getCanonicalType()
              ->isPointerType())
        poisoned.insert(param);
    }

    // Project each per-function region into the global union-find: all
    // bases of one region share a class, and an invalidated (escaping)
    // region poisons them.
    for (const clang::VarDecl *var : analysis.trackedVars()) {
      const PointerRegion *region = analysis.regionOf(var);
      if (!region)
        continue;
      const clang::VarDecl *first = nullptr;
      for (const PointerBaseBinding &binding : region->bases) {
        if (!first)
          first = binding.base;
        else
          unionFind.unite(first, binding.base);
        if (!region->invalidReason.empty())
          poisoned.insert(binding.base);
        // A compound-literal backing (C99-13) has no declaration
        // statement to anchor an owner struct at; any class containing
        // one stays on the Phase-1b slice lowering.
        if (literalTemps.isTemp(binding.base))
          poisoned.insert(binding.base);
      }
    }

    // Program-wide facts of pointer-typed globals (CTS-P4): merge this
    // body's region view of every tracked global pointer;
    // `importPointerGlobal` validates the union when the global itself is
    // imported (Pass B).
    for (const clang::VarDecl *var : analysis.trackedVars())
      if (!var->hasLocalStorage())
        if (const PointerRegion *region = analysis.regionOf(var))
          mergeRegionFacts(globalPtrFacts[var->getCanonicalDecl()], *region);

    // Program-wide member-pointer facts (CTS-P2): every body's bindings
    // and unresolvable field uses merge here; reads and writes of
    // data-pointer members consult the union at their use sites.
    for (const auto &entry : analysis.memberBindings())
      mergeMemberPointerFacts(entry.first, entry.second);
    for (const auto &entry : analysis.poisonedMemberFields())
      poisonedPtrFields.try_emplace(entry.first, entry.second);

    // Call edges: a pointer argument's root object unifies with the callee
    // definition's parameter; an unresolvable root poisons the parameter's
    // class. Callees without a definition in this TU add no edge — passing
    // a region to them stays on the Phase-1b call lowering, which composes
    // with a promoted base through its rewritten data place.
    forEachDataPointerCallArg(
        func->getBody(),
        [&](const clang::ParmVarDecl *param, const clang::Expr *argument) {
          if (const clang::VarDecl *root = resolveArgRoot(analysis, argument))
            unionFind.unite(root, param);
          else
            poisoned.insert(param);
        });
  }

  // Aggregate the classes. `find` compresses paths, so the node set is
  // snapshotted before aggregation.
  struct ClassInfo {
    SmallVector<const clang::VarDecl *, 2> storageBases;
    SmallVector<const clang::ParmVarDecl *, 4> params;
    bool poisoned = false;
  };
  llvm::DenseMap<const clang::VarDecl *, ClassInfo> classes;
  for (const clang::VarDecl *node : unionFind.nodes()) {
    ClassInfo &info = classes[unionFind.find(node)];
    if (poisoned.contains(node))
      info.poisoned = true;
    if (const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(node)) {
      if (isPointerType(param->getType()) &&
          !isFunctionPointer(param->getType()))
        info.params.push_back(param);
      continue;
    }
    if (!isPointerType(node->getType()) && node->hasLocalStorage())
      info.storageBases.push_back(node);
  }

  // Promote every class that satisfies the full rule; anything else is a
  // silent Phase-1b fallback.
  for (const auto &entry : classes) {
    const ClassInfo &info = entry.second;
    if (info.poisoned || info.params.empty() ||
        info.storageBases.size() != 1)
      continue;
    const clang::VarDecl *base = info.storageBases.front();
    const auto *owner = llvm::dyn_cast_if_present<clang::FunctionDecl>(
        base->getParentFunctionOrMethod());
    if (!owner)
      continue;
    const clang::ConstantArrayType *arrayType =
        astContext().getAsConstantArrayType(base->getType());
    // Local array of 1..32 elements: the struct_def `Default` derive MVP
    // limit. Scalar and oversized bases keep the Phase-1b lowering.
    if (!arrayType || arrayType->getSize().getZExtValue() == 0 ||
        arrayType->getSize().getZExtValue() > 32)
      continue;
    clang::QualType element = arrayType->getElementType();

    // Every unified parameter must belong to a defined function and point
    // at the base's element type.
    llvm::SmallPtrSet<const clang::FunctionDecl *, 4> methodFns;
    bool qualifies = true;
    for (const clang::ParmVarDecl *param : info.params) {
      const auto *fn =
          llvm::dyn_cast<clang::FunctionDecl>(param->getDeclContext());
      if (!fn || !fn->doesThisDeclarationHaveABody() ||
          !astContext().hasSameUnqualifiedType(
              element,
              param->getType().getCanonicalType()->getPointeeType())) {
        qualifies = false;
        break;
      }
      methodFns.insert(fn);
    }
    if (!qualifies)
      continue;
    for (const clang::FunctionDecl *fn : methodFns) {
      // All-or-nothing per function: every data-pointer parameter of the
      // function must resolve into this one class, the return type must be
      // a plain value, the function may not be the owner itself or C
      // `main`, and all of its call sites must be visible — an externally
      // visible function qualifies only when this TU is the whole program.
      if (fn == owner || fn->getName() == "main" ||
          (fn->isExternallyVisible() && !soleTranslationUnit) ||
          (isPointerType(fn->getReturnType()) &&
           !isFunctionPointer(fn->getReturnType()))) {
        qualifies = false;
        break;
      }
      for (const clang::ParmVarDecl *param : fn->parameters()) {
        if (isPointerType(param->getType()) &&
            !isFunctionPointer(param->getType()) &&
            unionFind.find(param) != entry.first) {
          qualifies = false;
          break;
        }
      }
      if (!qualifies)
        break;
    }
    if (!qualifies)
      continue;

    // The owner struct is named after the C spellings (`main`, not the
    // renamed `c_main`); an internal-linkage owning function takes the
    // per-TU tag so identically named statics never collide.
    std::string structName =
        (llvm::Twine("Owner_") +
         (owner->isExternallyVisible() ? "" : currentTuTag.c_str()) +
         owner->getName() + "_" + base->getName())
            .str();
    ownerPlans[base] = OwnerPlan{structName, /*structDefCreated=*/false};
    for (const clang::FunctionDecl *fn : methodFns)
      methodPlans[fn->getCanonicalDecl()] = base;
  }
}

//===----------------------------------------------------------------------===//
// Cell-slice planning (CTS-P10 Pass A)
//===----------------------------------------------------------------------===//

namespace {

/// Recursive body walk collecting the per-parameter facts the cell-slice
/// qualification needs: which data-pointer parameters are null-checked
/// (compared against a null pointer constant, logically negated, or truth
/// tested as a statement condition) and which are used in any shape other
/// than a subscript base, a dereference base, a null check, or an argument
/// to a defined non-variadic callee (such "escaping" uses poison the
/// parameter's class — the cell-slice emission only models the whitelisted
/// shapes). The walk is top-down: a consuming context skips the consumed
/// parameter read, so any parameter read reached raw is an escape.
class CellSliceBodyScan {
public:
  CellSliceBodyScan(
      clang::ASTContext &context,
      llvm::SmallPtrSetImpl<const clang::ParmVarDecl *> &nullChecked,
      llvm::SmallPtrSetImpl<const clang::ParmVarDecl *> &poisoned)
      : context(context), nullChecked(nullChecked), poisoned(poisoned) {}

  /// Walks `stmt` and its children.
  void visit(const clang::Stmt *stmt) {
    if (!stmt)
      return;

    // Statement conditions truth-test a directly named pointer parameter
    // (`if (p)`, `while (p)`): a null check.
    if (const auto *ifStmt = llvm::dyn_cast<clang::IfStmt>(stmt))
      markConditionNullCheck(ifStmt->getCond());
    else if (const auto *whileStmt = llvm::dyn_cast<clang::WhileStmt>(stmt))
      markConditionNullCheck(whileStmt->getCond());
    else if (const auto *doStmt = llvm::dyn_cast<clang::DoStmt>(stmt))
      markConditionNullCheck(doStmt->getCond());
    else if (const auto *forStmt = llvm::dyn_cast<clang::ForStmt>(stmt))
      markConditionNullCheck(forStmt->getCond());

    if (const auto *expr = llvm::dyn_cast<clang::Expr>(stmt)) {
      visitExpr(expr);
      return;
    }
    for (const clang::Stmt *child : stmt->children())
      visit(child);
  }

private:
  /// Returns the data-pointer parameter `expr` reads, or null.
  const clang::ParmVarDecl *asParamRead(const clang::Expr *expr) const {
    if (!expr)
      return nullptr;
    const clang::Expr *e = stripTrivia(expr);
    while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
      if (cast->getCastKind() != clang::CK_LValueToRValue &&
          cast->getCastKind() != clang::CK_NoOp)
        break;
      e = stripTrivia(cast->getSubExpr());
    }
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
    const auto *param =
        ref ? llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()) : nullptr;
    if (param && isPointerType(param->getType()) &&
        !isFunctionPointer(param->getType()))
      return param;
    return nullptr;
  }

  /// Returns whether `expr` is a null pointer constant.
  bool isNullConstant(const clang::Expr *expr) const {
    return expr->isNullPointerConstant(
               context, clang::Expr::NPC_NeverValueDependent) !=
           clang::Expr::NPCK_NotNull;
  }

  /// Marks a statement condition that is a bare parameter read (a truth
  /// test) as a null check and lets the regular walk handle the rest.
  void markConditionNullCheck(const clang::Expr *cond) {
    if (const clang::ParmVarDecl *param = asParamRead(cond)) {
      nullChecked.insert(param);
      conditionTested.insert(param);
    }
  }

  /// Expression walk with consuming-context dispatch.
  void visitExpr(const clang::Expr *expr) {
    const clang::Expr *e = stripTrivia(expr);
    // A subscript through a parameter consumes the base read.
    if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e)) {
      if (asParamRead(subscript->getBase())) {
        visitExpr(subscript->getIdx());
        return;
      }
    }
    // A dereference of a parameter consumes the read; `!p` is a null
    // check.
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e)) {
      const clang::ParmVarDecl *param = asParamRead(unary->getSubExpr());
      if (param && unary->getOpcode() == clang::UO_Deref)
        return;
      if (param && unary->getOpcode() == clang::UO_LNot) {
        nullChecked.insert(param);
        return;
      }
    }
    // `p == NULL` / `p != NULL` is a null check; comparisons against
    // anything else fall through to the raw-read poison below.
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e)) {
      if (binary->getOpcode() == clang::BO_EQ ||
          binary->getOpcode() == clang::BO_NE) {
        const clang::ParmVarDecl *lhsParam = asParamRead(binary->getLHS());
        const clang::ParmVarDecl *rhsParam = asParamRead(binary->getRHS());
        if (lhsParam && isNullConstant(binary->getRHS())) {
          nullChecked.insert(lhsParam);
          return;
        }
        if (rhsParam && isNullConstant(binary->getLHS())) {
          nullChecked.insert(rhsParam);
          return;
        }
      }
    }
    // A defined non-variadic direct callee consumes its parameter-read
    // arguments (the call-edge scan classifies them); any other call
    // escapes them.
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
      const clang::FunctionDecl *callee = call->getDirectCallee();
      const clang::FunctionDecl *definition =
          callee ? callee->getDefinition() : nullptr;
      bool visible = callee && !callee->isVariadic() && definition &&
                     definition->hasBody() &&
                     call->getNumArgs() == definition->getNumParams();
      for (const clang::Expr *argument : call->arguments()) {
        if (const clang::ParmVarDecl *param = asParamRead(argument)) {
          if (!visible)
            poisoned.insert(param);
          continue;
        }
        visitExpr(argument);
      }
      return;
    }
    // A raw parameter read outside every whitelisted context escapes the
    // class (walks, reassignments, copies, differences, address-taking,
    // returns, ...). A bare read already recorded as a statement-condition
    // truth test is consumed.
    if (const clang::ParmVarDecl *param = asParamRead(e)) {
      if (!conditionTested.contains(param))
        poisoned.insert(param);
      return;
    }
    // The address-of or ++/-- of a parameter never reads it
    // (no LValueToRValue), so catch the raw DeclRefExpr too.
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
      if (const auto *param =
              llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()))
        if (isPointerType(param->getType()) &&
            !isFunctionPointer(param->getType()))
          poisoned.insert(param);
      return;
    }
    for (const clang::Stmt *child : e->children())
      visit(child);
  }

  clang::ASTContext &context;
  llvm::SmallPtrSetImpl<const clang::ParmVarDecl *> &nullChecked;
  llvm::SmallPtrSetImpl<const clang::ParmVarDecl *> &poisoned;
  /// Parameters truth-tested as a whole statement condition: their bare
  /// read is consumed, not an escape.
  llvm::SmallPtrSet<const clang::ParmVarDecl *, 4> conditionTested;
};

} // namespace

const clang::VarDecl *
CImporter::asDecayedGlobalArrayArg(const clang::Expr *expr) const {
  const clang::Expr *e = stripTrivia(expr);
  while (const auto *noop = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    if (noop->getCastKind() != clang::CK_NoOp)
      break;
    e = stripTrivia(noop->getSubExpr());
  }
  const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(e);
  if (!decay || decay->getCastKind() != clang::CK_ArrayToPointerDecay)
    return nullptr;
  const auto *ref =
      llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(decay->getSubExpr()));
  const auto *var = ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl())
                        : nullptr;
  if (!var || var->hasLocalStorage() ||
      !astContext().getAsConstantArrayType(var->getType()))
    return nullptr;
  return var->getCanonicalDecl();
}

const clang::ParmVarDecl *
CImporter::asPointerParamRead(const clang::Expr *expr) const {
  const clang::Expr *e = stripTrivia(expr);
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    if (cast->getCastKind() != clang::CK_LValueToRValue &&
        cast->getCastKind() != clang::CK_NoOp)
      break;
    e = stripTrivia(cast->getSubExpr());
  }
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
  const auto *param =
      ref ? llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()) : nullptr;
  if (param && isPointerType(param->getType()) &&
      !isFunctionPointer(param->getType()))
    return param;
  return nullptr;
}

void CImporter::planCellSlices(const clang::TranslationUnitDecl *unit,
                               bool soleTranslationUnit) {
  // Union-find over data-pointer parameters and global array bases,
  // mirroring `planOwners`' machinery (see `VarDeclUnionFind` for the
  // fixpoint argument).
  VarDeclUnionFind unionFind;

  llvm::SmallPtrSet<const clang::VarDecl *, 8> poisoned;
  llvm::SmallPtrSet<const clang::ParmVarDecl *, 8> nullChecked;
  // The first local object a class's parameter also received (the Mixed
  // boundary), keyed by the callee parameter that received it.
  llvm::DenseMap<const clang::VarDecl *, std::string> localJoins;

  for (const clang::FunctionDecl *func :
       collectPassAFunctionDefinitions(unit)) {
    // Body facts: null checks and escaping uses of this definition's own
    // data-pointer parameters.
    llvm::SmallPtrSet<const clang::ParmVarDecl *, 4> bodyNullChecked;
    llvm::SmallPtrSet<const clang::ParmVarDecl *, 4> bodyPoisoned;
    CellSliceBodyScan scan(astContext(), bodyNullChecked, bodyPoisoned);
    scan.visit(func->getBody());
    nullChecked.insert(bodyNullChecked.begin(), bodyNullChecked.end());
    for (const clang::ParmVarDecl *param : bodyPoisoned)
      poisoned.insert(param);

    // A pointer local bound to a parameter joins its region (Phase 1a
    // records the parameter as a base); the cell-slice emission has no
    // local-pointer form, so such a parameter's class stays on the
    // historical lowering.
    PointerRegionAnalysis analysis;
    analysis.literalTemps = &literalTemps;
    analysis.analyze(astContext(), func->getBody());
    for (const clang::VarDecl *var : analysis.trackedVars())
      if (const PointerRegion *region = analysis.regionOf(var))
        for (const PointerBaseBinding &binding : region->bases)
          if (llvm::isa<clang::ParmVarDecl>(binding.base))
            poisoned.insert(binding.base);

    // In a project import an externally visible function may be called
    // from an unseen TU with a local argument; its class must not turn
    // its parameters into cell-slices.
    if (!soleTranslationUnit && func->isExternallyVisible())
      for (const clang::ParmVarDecl *param : func->parameters())
        if (isPointerType(param->getType()) &&
            !isFunctionPointer(param->getType()))
          poisoned.insert(param);

    // Call edges: exactly two argument shapes bind into the class — the
    // direct decay of a global array and the forwarding of another
    // data-pointer parameter. A local array decay records the Mixed
    // boundary fact; everything else poisons the callee parameter.
    forEachDataPointerCallArg(
        func->getBody(), [&](const clang::ParmVarDecl *calleeParam,
                             const clang::Expr *argument) {
          (void)unionFind.find(calleeParam);
          if (calleeParam->getType()
                  .getCanonicalType()
                  ->getPointeeType()
                  .getCanonicalType()
                  ->isPointerType()) {
            poisoned.insert(calleeParam);
            return;
          }
          if (const clang::VarDecl *global =
                  asDecayedGlobalArrayArg(argument)) {
            unionFind.unite(calleeParam, global);
            return;
          }
          if (const clang::ParmVarDecl *forwarded =
                  asPointerParamRead(argument)) {
            unionFind.unite(calleeParam, forwarded);
            return;
          }
          // A directly decayed local array is the ordinary Phase-1b slice
          // argument; record it as the Mixed boundary witness in case the
          // class also picks up a global base.
          const clang::Expr *e = stripTrivia(argument);
          const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(e);
          const auto *ref =
              decay && decay->getCastKind() == clang::CK_ArrayToPointerDecay
                  ? llvm::dyn_cast<clang::DeclRefExpr>(
                        stripTrivia(decay->getSubExpr()))
                  : nullptr;
          const auto *localVar =
              ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
          if (localVar && localVar->hasLocalStorage()) {
            localJoins.try_emplace(calleeParam, localVar->getName().str());
            return;
          }
          poisoned.insert(calleeParam);
        });
  }

  // Aggregate the classes (snapshotting the nodes: `find` compresses
  // paths).
  struct ClassInfo {
    SmallVector<const clang::VarDecl *, 2> globals;
    SmallVector<const clang::ParmVarDecl *, 4> params;
    std::string localName;
    bool poisoned = false;
    bool nullChecked = false;
  };
  llvm::DenseMap<const clang::VarDecl *, ClassInfo> classes;
  for (const clang::VarDecl *node : unionFind.nodes()) {
    ClassInfo &info = classes[unionFind.find(node)];
    if (poisoned.contains(node))
      info.poisoned = true;
    if (const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(node)) {
      info.params.push_back(param);
      if (nullChecked.contains(param))
        info.nullChecked = true;
      auto joined = localJoins.find(param);
      if (joined != localJoins.end() && info.localName.empty())
        info.localName = joined->second;
      continue;
    }
    if (!node->hasLocalStorage() && !isPointerType(node->getType()))
      info.globals.push_back(node);
  }

  // Qualify or record the located boundary per class. Classes without a
  // global base are ordinary Phase-1b classes; classes with one that hit
  // any other snag silently keep the historical staged-copy rejection.
  for (const auto &entry : classes) {
    const ClassInfo &info = entry.second;
    if (info.params.empty() || info.globals.empty() || info.poisoned)
      continue;
    if (!info.localName.empty()) {
      for (const clang::VarDecl *global : info.globals)
        cellSliceRejects.try_emplace(
            global, CellSliceReject{CellSliceReject::Kind::Mixed,
                                    global->getName().str(), info.localName});
      continue;
    }
    if (info.nullChecked) {
      for (const clang::VarDecl *global : info.globals)
        cellSliceRejects.try_emplace(
            global, CellSliceReject{CellSliceReject::Kind::NullableGlobal,
                                    global->getName().str(), std::string()});
      continue;
    }
    // Every base must be a mutable (non-const, and internal unless this
    // TU is the whole program) one-dimensional global array of one shared
    // supported scalar element type.
    bool qualifies = true;
    clang::QualType element;
    for (const clang::VarDecl *global : info.globals) {
      const clang::ConstantArrayType *arrayType =
          astContext().getAsConstantArrayType(global->getType());
      if (!arrayType || global->getType().isConstQualified() ||
          (!soleTranslationUnit && global->isExternallyVisible())) {
        qualifies = false;
        break;
      }
      clang::QualType elem = arrayType->getElementType();
      bool scalarElem =
          elem->isRealFloatingType() ||
          (elem->isIntegerType() && !elem->isEnumeralType() &&
           !elem->isBooleanType());
      if (elem.isConstQualified() || astContext().getAsArrayType(elem) ||
          !scalarElem) {
        qualifies = false;
        break;
      }
      if (element.isNull())
        element = elem;
      else if (!astContext().hasSameUnqualifiedType(element, elem))
        qualifies = false;
      if (!qualifies)
        break;
    }
    // Every parameter must belong to a defined (and internal, unless sole
    // TU) function and point at the shared element type.
    for (const clang::ParmVarDecl *param : info.params) {
      if (!qualifies)
        break;
      const auto *fn =
          llvm::dyn_cast<clang::FunctionDecl>(param->getDeclContext());
      if (!fn || !fn->doesThisDeclarationHaveABody() ||
          fn->getName() == "main" ||
          (!soleTranslationUnit && fn->isExternallyVisible()) ||
          !astContext().hasSameUnqualifiedType(
              element,
              param->getType().getCanonicalType()->getPointeeType()))
        qualifies = false;
    }
    if (!qualifies)
      continue;
    for (const clang::ParmVarDecl *param : info.params)
      cellSliceParams.insert(param);
  }
}

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

void CImporter::collectAddressTaken(const clang::Stmt *stmt) {
  if (!stmt)
    return;
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt))
    if (unary->getOpcode() == clang::UO_AddrOf)
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
              unary->getSubExpr()->IgnoreParens()))
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
          addressTaken.insert(var);
  for (const clang::Stmt *child : stmt->children())
    collectAddressTaken(child);
}

void CImporter::planFnPtrAliases(const clang::TranslationUnitDecl *unit) {
  fnPtrGlobalsWritten.clear();
  addressTakenFunctions.clear();

  // Records a write (assignment, increment) or escape (address-of) of a
  // file-scope function-pointer variable: such a variable never aliases.
  auto markWritten = [&](const clang::Expr *expr) {
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stripTrivia(expr));
    if (!ref)
      return;
    const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
    if (!var || !var->hasGlobalStorage() ||
        !var->getType().getCanonicalType()->isFunctionPointerType())
      return;
    fnPtrGlobalsWritten.insert(
        llvm::cast<clang::VarDecl>(var->getCanonicalDecl()));
  };

  // Walks one statement/expression subtree. A `DeclRefExpr` naming a
  // function ANYWHERE outside the callee position of a direct call is an
  // address-taking use (the C decay model: the reference becomes a
  // function pointer value), so it joins the candidate set consulted by
  // `classifyFnPtrPointerResult`.
  auto scanStmt = [&](auto &&self, const clang::Stmt *stmt) -> void {
    if (!stmt)
      return;
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
      // The callee of a direct call is not a value use of the function;
      // a function-pointer callee expression is scanned like any value.
      if (!call->getDirectCallee())
        self(self, call->getCallee());
      for (const clang::Expr *argument : call->arguments())
        self(self, argument);
      return;
    }
    if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
      for (const clang::Decl *decl : declStmt->decls())
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
          self(self, var->getInit());
      return;
    }
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt)) {
      if (const auto *fn =
              llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl())) {
        const auto *canonical =
            llvm::cast<clang::FunctionDecl>(fn->getCanonicalDecl());
        if (!llvm::is_contained(addressTakenFunctions, canonical))
          addressTakenFunctions.push_back(canonical);
      }
      return;
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt))
      if (binary->isAssignmentOp())
        markWritten(binary->getLHS());
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt))
      if (unary->getOpcode() == clang::UO_AddrOf ||
          unary->isIncrementDecrementOp())
        markWritten(unary->getSubExpr());
    for (const clang::Stmt *child : stmt->children())
      self(self, child);
  };

  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (func->hasBody() && func->getDefinition() == func)
        scanStmt(scanStmt, func->getBody());
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
      if (const clang::Expr *init = var->getInit())
        scanStmt(scanStmt, init);
  }

  // Alias planning: a file-scope function pointer initialized to a known
  // function and never written anywhere in the TU. An externally visible
  // variable only aliases in a sole-TU import (another TU could rebind
  // it); a variadic target only when it is the hosted definition-less
  // printf/fprintf, whose calls route through the printf machinery.
  for (const clang::Decl *decl : unit->decls()) {
    const auto *var = llvm::dyn_cast<clang::VarDecl>(decl);
    if (!var || var->isImplicit() || isSystemHeaderDecl(var))
      continue;
    if (!var->getType().getCanonicalType()->isFunctionPointerType())
      continue;
    const auto *canonical =
        llvm::cast<clang::VarDecl>(var->getCanonicalDecl());
    if (fnPtrAliases.contains(canonical) ||
        fnPtrGlobalsWritten.contains(canonical))
      continue;
    if (!currentSoleTU && var->isExternallyVisible())
      continue;
    const clang::Expr *init = canonical->getAnyInitializer();
    const clang::Expr *fnExpr = init ? returnedFunctionExpr(init) : nullptr;
    if (!fnExpr)
      continue;
    const auto *target = llvm::cast<clang::FunctionDecl>(
        llvm::cast<clang::DeclRefExpr>(fnExpr)->getDecl());
    if (target->isVariadic()) {
      if (target->getDefinition() || !target->getDeclName().isIdentifier())
        continue;
      llvm::StringRef name = target->getName();
      if (name != "printf" && name != "fprintf")
        continue;
    }
    fnPtrAliases[canonical] = target;
  }
}

LogicalResult
CImporter::planCursorParams(const clang::TranslationUnitDecl *unit) {
  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (!func || !func->isThisDeclarationADefinition() || !func->hasBody())
      continue;
    // C main's `char **argv` has its own policy (dropped from the
    // imported signature; uses rejected) — never a string cursor.
    if (func->isMain())
      continue;
    SmallVector<const clang::ParmVarDecl *, 2> eligible;
    for (const clang::ParmVarDecl *param : func->parameters())
      if (isCharPointerPointerType(param->getType()))
        eligible.push_back(param);
    if (eligible.empty())
      continue;
    // The bounded shape: the parameter appears only under its own
    // dereference — reads and the `*s = p` advancement. Everything else
    // (stored, passed on, address-taken, content writes) escapes.
    for (const clang::ParmVarDecl *param : eligible)
      if (const clang::Expr *escape =
              findCursorParamEscape(func->getBody(), param))
        return emitError(translateLoc(escape->getBeginLoc()))
               << "unsupported: pointer-to-pointer parameter escapes the "
                  "string-cursor shape";
    // Region check: a write through a pointer DERIVED from the cursor
    // parameter (`p = *s; *p = c;`) writes region content the shared
    // slice lowering cannot accept.
    llvm::SmallPtrSet<const clang::ParmVarDecl *, 2> candidates(
        eligible.begin(), eligible.end());
    PointerRegionAnalysis analysis;
    analysis.cursorParamQuery = [&](const clang::ParmVarDecl *param) {
      return candidates.contains(param);
    };
    analysis.analyze(astContext(), func->getBody());
    SmallVector<const clang::VarDecl *, 8> locals;
    collectLocalPointerDecls(func->getBody(), locals);
    for (const clang::VarDecl *var : locals) {
      const PointerRegion *region = analysis.regionOf(var);
      if (!region || !region->hasWriteThrough)
        continue;
      for (const PointerBaseBinding &binding : region->bases)
        if (const auto *param =
                llvm::dyn_cast_if_present<clang::ParmVarDecl>(binding.base);
            param && candidates.contains(param))
          return emitError(translateLoc(region->writeThroughLoc))
                 << "unsupported: write through a string-cursor parameter";
    }
    for (const clang::ParmVarDecl *param : eligible)
      cursorParams.insert(param);
  }
  return success();
}

LogicalResult
CImporter::planVaMonomorph(const clang::TranslationUnitDecl *unit) {
  // Gather this TU's va_list-using variadic definitions.
  SmallVector<const clang::FunctionDecl *, 4> targets;
  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (!func || !func->isThisDeclarationADefinition() || !func->hasBody() ||
        !func->isVariadic())
      continue;
    if (bodyUsesVaList(astContext(), func->getBody()))
      targets.push_back(func);
  }
  if (targets.empty())
    return success();

  // Scope checks per definition: no va_copy, and the va_list objects may
  // only ever feed va_start/va_end/va_arg — a va_list passed to any
  // callee escapes the definition (the callee would consume varargs the
  // monomorphizer cannot see). Checked BEFORE any declaration imports,
  // so this diagnostic beats the callee's va_list-parameter rejection.
  clang::QualType vaListType =
      astContext().getBuiltinVaListType().getCanonicalType();
  for (const clang::FunctionDecl *func : targets) {
    llvm::SmallPtrSet<const clang::Expr *, 8> consumed;
    SmallVector<const clang::Stmt *> worklist{func->getBody()};
    while (!worklist.empty()) {
      const clang::Stmt *current = worklist.pop_back_val();
      if (!current)
        continue;
      if (const auto *call = llvm::dyn_cast<clang::CallExpr>(current)) {
        switch (call->getBuiltinCallee()) {
        case clang::Builtin::BI__builtin_va_copy:
        case clang::Builtin::BI__builtin_ms_va_copy:
        case clang::Builtin::BIva_copy:
          return emitError(translateLoc(call->getBeginLoc()))
                 << "unsupported: va_copy";
        case clang::Builtin::BI__builtin_va_start:
        case clang::Builtin::BI__builtin_c23_va_start:
        case clang::Builtin::BI__builtin_ms_va_start:
        case clang::Builtin::BI__va_start:
        case clang::Builtin::BIva_start:
        case clang::Builtin::BI__builtin_va_end:
        case clang::Builtin::BI__builtin_ms_va_end:
        case clang::Builtin::BIva_end:
          if (call->getNumArgs() >= 1)
            consumed.insert(strippedImplicitRef(call->getArg(0)));
          break;
        default:
          break;
        }
      }
      if (const auto *vaArg = llvm::dyn_cast<clang::VAArgExpr>(current))
        consumed.insert(strippedImplicitRef(vaArg->getSubExpr()));
      for (const clang::Stmt *child : current->children())
        worklist.push_back(child);
    }
    worklist.push_back(func->getBody());
    while (!worklist.empty()) {
      const clang::Stmt *current = worklist.pop_back_val();
      if (!current)
        continue;
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(current)) {
        const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
        if (var &&
            astContext().hasSameType(var->getType().getCanonicalType(),
                                     vaListType) &&
            !consumed.contains(ref))
          return emitError(translateLoc(ref->getBeginLoc()))
                 << "unsupported: va_list escapes variadic definition";
      }
      for (const clang::Stmt *child : current->children())
        worklist.push_back(child);
    }
  }

  // Address-of scan and call-site enumeration over the whole TU, in
  // declaration order (pre-order within each body), so clone numbering is
  // deterministic. A reference to a monomorphized definition outside a
  // direct-callee position makes its call sites non-enumerable.
  llvm::DenseMap<const clang::FunctionDecl *, const clang::FunctionDecl *>
      canonicalTargets;
  for (const clang::FunctionDecl *func : targets)
    canonicalTargets[func->getCanonicalDecl()] = func;
  struct SiteRecord {
    const clang::CallExpr *call;
    const clang::FunctionDecl *target;
  };
  SmallVector<SiteRecord, 8> sites;
  llvm::SmallPtrSet<const clang::Expr *, 16> calleeRefs;
  std::function<LogicalResult(const clang::Stmt *)> scan =
      [&](const clang::Stmt *stmt) -> LogicalResult {
    if (!stmt)
      return success();
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
      const clang::FunctionDecl *callee = call->getDirectCallee();
      const clang::FunctionDecl *target =
          callee ? canonicalTargets.lookup(callee->getCanonicalDecl())
                 : nullptr;
      if (target) {
        sites.push_back({call, target});
        calleeRefs.insert(strippedImplicitRef(call->getCallee()));
      }
    }
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
      if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()))
        if (canonicalTargets.count(fn->getCanonicalDecl()) &&
            !calleeRefs.contains(ref))
          return emitError(translateLoc(ref->getBeginLoc()))
                 << "unsupported: address of variadic definition";
    for (const clang::Stmt *child : stmt->children())
      if (failed(scan(child)))
        return failure();
    return success();
  };
  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (func->isThisDeclarationADefinition() && func->hasBody() &&
          failed(scan(func->getBody())))
        return failure();
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
      if (var->hasInit() && failed(scan(var->getInit())))
        return failure();
  }

  // One clone per distinct extras signature; call sites record their
  // clone index. Every target gets a plan entry — a site-less definition
  // plans zero clones and drops entirely at import.
  for (const clang::FunctionDecl *func : targets)
    (void)vaMonomorphPlans[func->getCanonicalDecl()];
  for (const SiteRecord &site : sites) {
    VaMonomorphPlan &plan =
        vaMonomorphPlans[site.target->getCanonicalDecl()];
    unsigned named = site.target->getNumParams();
    if (site.call->getNumArgs() < named)
      return emitError(translateLoc(site.call->getBeginLoc()))
             << "unsupported: call argument count mismatch";
    SmallVector<Type, 4> extraTypes;
    for (unsigned index = named; index < site.call->getNumArgs(); ++index) {
      const clang::Expr *argument = site.call->getArg(index);
      Location argLoc = translateLoc(argument->getBeginLoc());
      // Extras pass BY VALUE (clang has already applied the default
      // argument promotions); a data-pointer extra has no by-value
      // representation under the decomposition.
      if (isDataPointer(argument->getType()))
        return emitError(argLoc)
               << "unsupported: pointer argument to a variadic call";
      FailureOr<Type> mapped = mapType(argument->getType(), argLoc);
      if (failed(mapped))
        return failure();
      extraTypes.push_back(*mapped);
    }
    unsigned cloneIndex = plan.clones.size();
    for (auto [index, clone] : llvm::enumerate(plan.clones))
      if (clone.extraTypes == extraTypes) {
        cloneIndex = static_cast<unsigned>(index);
        break;
      }
    if (cloneIndex == plan.clones.size()) {
      // Clone names carry the original symbol plus a per-signature
      // suffix; the original bare symbol is never emitted.
      std::string name = mlirFuncName(site.target) + "__" +
                         std::to_string(plan.clones.size() + 1);
      while (ordinaryNameTaken(name) || functions.lookup(name))
        name += "_";
      plan.clones.push_back(VaClonePlan{name, extraTypes});
    }
    vaCallSiteClones[site.call] = cloneIndex;
  }
  return success();
}

//===----------------------------------------------------------------------===//
// CTS-BR (00216): `void *` fn-ptr struct members.
//===----------------------------------------------------------------------===//

void CImporter::collectDeclTypeRecords(const clang::TranslationUnitDecl *unit) {
  declTypeUsedRecords.clear();
  clang::ASTContext &context = astContext();
  auto noteType = [&](clang::QualType type) {
    clang::QualType t = type.getCanonicalType();
    while (true) {
      if (const clang::ArrayType *array = context.getAsArrayType(t)) {
        t = array->getElementType().getCanonicalType();
        continue;
      }
      if (t->isPointerType()) {
        t = t->getPointeeType().getCanonicalType();
        continue;
      }
      break;
    }
    if (const auto *record = t->getAsRecordDecl())
      if (const clang::RecordDecl *definition = record->getDefinition())
        declTypeUsedRecords.insert(definition);
  };
  auto scanStmt = [&](auto &&self, const clang::Stmt *stmt) -> void {
    if (!stmt)
      return;
    if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt))
      for (const clang::Decl *decl : declStmt->decls())
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
          noteType(var->getType());
    for (const clang::Stmt *child : stmt->children())
      self(self, child);
  };
  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      noteType(func->getReturnType());
      for (const clang::ParmVarDecl *param : func->parameters())
        noteType(param->getType());
      if (func->hasBody() && func->getDefinition() == func)
        scanStmt(scanStmt, func->getBody());
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
      noteType(var->getType());
      continue;
    }
    if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(decl))
      if (const clang::RecordDecl *definition = record->getDefinition())
        // A byte-region record never materializes its members as typed
        // fields, so its field types are not declaration-type uses.
        if (!isByteRegionRecord(definition))
          for (const clang::FieldDecl *field : definition->fields())
            noteType(field->getType());
  }
}

void CImporter::planFnPtrMembers(const clang::TranslationUnitDecl *unit) {
  clang::ASTContext &context = astContext();
  llvm::DenseMap<const clang::FieldDecl *, const clang::FunctionDecl *>
      targets;
  llvm::DenseSet<const clang::FieldDecl *> disqualified;
  llvm::SmallVector<std::pair<const clang::FieldDecl *, clang::QualType>, 4>
      readerCasts;

  auto isCandidateField = [&](const clang::FieldDecl *field) -> bool {
    if (!field)
      return false;
    clang::QualType type = field->getType().getCanonicalType();
    return type->isPointerType() && type->getPointeeType()->isVoidType();
  };

  // Strips the value trivia around a stored function address: implicit
  // and explicit casts (function-to-pointer decay, the void* conversion)
  // and the optional address-of.
  auto functionTarget =
      [&](const clang::Expr *expr) -> const clang::FunctionDecl * {
    const clang::Expr *e = expr->IgnoreParenCasts();
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
      if (unary->getOpcode() == clang::UO_AddrOf)
        e = unary->getSubExpr()->IgnoreParenCasts();
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
    return ref ? llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()) : nullptr;
  };

  // Walks an aggregate initializer, recording (or disqualifying) every
  // value stored into a candidate member.
  auto walkInit = [&](auto &&self, clang::QualType type,
                      const clang::Expr *init) -> void {
    if (!init)
      return;
    const clang::Expr *e = init->IgnoreParenImpCasts();
    if (const auto *compound = llvm::dyn_cast<clang::CompoundLiteralExpr>(e)) {
      self(self, compound->getType(), compound->getInitializer());
      return;
    }
    const auto *list = llvm::dyn_cast<clang::InitListExpr>(e);
    if (!list)
      return;
    if (const clang::InitListExpr *semantic = list->getSemanticForm())
      list = semantic;
    clang::QualType canonical = type.getCanonicalType();
    if (const clang::ConstantArrayType *array =
            context.getAsConstantArrayType(canonical)) {
      for (unsigned i = 0, n = list->getNumInits(); i != n; ++i)
        self(self, array->getElementType(), list->getInit(i));
      if (list->hasArrayFiller())
        self(self, array->getElementType(), list->getArrayFiller());
      return;
    }
    const auto *record = canonical->getAsRecordDecl();
    const clang::RecordDecl *definition =
        record ? record->getDefinition() : nullptr;
    if (!definition)
      return;
    if (definition->isUnion()) {
      if (const clang::FieldDecl *active = list->getInitializedFieldInUnion();
          active && list->getNumInits())
        self(self, active->getType(), list->getInit(0));
      return;
    }
    unsigned index = 0;
    for (const clang::FieldDecl *field : definition->fields()) {
      if (index >= list->getNumInits())
        break;
      const clang::Expr *element = list->getInit(index++);
      if (!element)
        continue;
      if (!isCandidateField(field)) {
        self(self, field->getType(), element);
        continue;
      }
      if (llvm::isa<clang::ImplicitValueInitExpr>(element) ||
          element->isNullPointerConstant(
              context, clang::Expr::NPC_NeverValueDependent) !=
              clang::Expr::NPCK_NotNull)
        continue; // The null constant folds to None.
      const clang::FunctionDecl *target = functionTarget(element);
      if (!target || target->isVariadic() ||
          !target->getType()->getAs<clang::FunctionProtoType>()) {
        disqualified.insert(field);
        continue;
      }
      auto [it, inserted] = targets.try_emplace(field, target);
      if (!inserted &&
          !context.hasSameType(it->second->getType(), target->getType()))
        disqualified.insert(field);
    }
  };

  // Walks a function body: a candidate-member read under a cast to a
  // function-pointer type is the one admitted use; any other mention
  // disqualifies.
  auto scanStmt = [&](auto &&self, const clang::Stmt *stmt) -> void {
    if (!stmt)
      return;
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(stmt)) {
      if (isFunctionPointer(cast->getType())) {
        const clang::Expr *sub = cast->getSubExpr()->IgnoreParenImpCasts();
        if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(sub)) {
          if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(
                  member->getMemberDecl());
              field && isCandidateField(field)) {
            readerCasts.push_back({field, cast->getType()});
            self(self, member->getBase());
            return;
          }
        }
      }
    }
    if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
      for (const clang::Decl *decl : declStmt->decls())
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
          walkInit(walkInit, var->getType(), var->getInit());
      // Fall through: the generic child scan below revisits the
      // initializer expressions, which mention no candidate members in
      // admitted programs (a mention there disqualifies, as intended).
    }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(stmt))
      if (const auto *field =
              llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
          field && isCandidateField(field))
        disqualified.insert(field);
    for (const clang::Stmt *child : stmt->children())
      self(self, child);
  };

  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl))
      continue;
    if (const auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (func->hasBody() && func->getDefinition() == func)
        scanStmt(scanStmt, func->getBody());
      continue;
    }
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
      walkInit(walkInit, var->getType(), var->getInit());
  }

  for (auto &[field, target] : targets) {
    if (disqualified.contains(field))
      continue;
    bool compatible = true;
    for (auto &[readField, castType] : readerCasts)
      if (readField == field &&
          !context.typesAreCompatible(
              castType.getCanonicalType()->getPointeeType(),
              target->getType()))
        compatible = false;
    if (!compatible)
      continue;
    fnPtrMemberTypes[field] = context.getPointerType(target->getType());
  }
}

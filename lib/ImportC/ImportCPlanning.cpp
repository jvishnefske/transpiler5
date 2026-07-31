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
///
/// FR-53 (recoverable Pass-A rejections). Only TWO of these planners can
/// reject at all: `planCursorParams` and `planVaMonomorph`. The other five
/// return `void` — they are strictly additive analyses that either prove a
/// promotion and record it or leave the construct on its ordinary path, so
/// they have no rejection to recover. Because the planners run before the
/// declaration walk, their failures used to bypass FR-42's recovery entirely
/// and take the whole translation unit down with them; every rejection site
/// in both is a fact about ONE declaration, so under `recoverFromRejections`
/// each is credited to that declaration (`recoverPlannerRejection`) and the
/// walk drops or stubs it like any other unsupported item. With recovery off
/// nothing here behaves differently: the first rejection still prints as an
/// error and still fails the import.
//
//===----------------------------------------------------------------------===//

#include "CImporterInternal.h"

using namespace mlir;

/// The owner-struct MVP limit on a promoted local array's element count:
/// the ceiling `struct_def`'s `Default` derive tolerates. Named so
/// `planOwners` and `planArrayMemberPointers` (which reuses a class only
/// `planOwners` already promoted, so this ceiling is inherited rather than
/// re-checked in the common case) share one definition instead of two
/// independent literal `32`s.
static constexpr unsigned kMaxOwnerArrayElements = 32;

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

  // W3.3 (G3): an externally visible function that is NOT the whole program
  // can still be promoted when the whole-program facts prove this TU sees all
  // its call sites — i.e. no TU other than this one references it (calls it or
  // takes its address). Promotion requires a direct call in THIS TU to bind
  // the base, so the one referencing TU is necessarily this one; if a second
  // TU calls it or takes its address, an argument this TU never analyzed could
  // reach the function and break the all-or-nothing per-function unification,
  // so the class must stay on the Phase-1b slice fallback. `wholeProgram` is
  // empty for a single-file import, where `soleTranslationUnit` is already
  // true and this helper is never consulted.
  auto externalFnFullyVisible = [&](const clang::FunctionDecl *fn) -> bool {
    std::string sym = mlirFuncName(fn);
    int seenTu = -1;
    auto onlyOneTu = [&](const llvm::SmallVectorImpl<unsigned> &tus) -> bool {
      for (unsigned tu : tus) {
        if (seenTu < 0)
          seenTu = static_cast<int>(tu);
        else if (static_cast<int>(tu) != seenTu)
          return false;
      }
      return true;
    };
    auto callIt = wholeProgram.calleeToCallerTus.find(sym);
    if (callIt != wholeProgram.calleeToCallerTus.end() &&
        !onlyOneTu(callIt->second))
      return false;
    auto addrIt = wholeProgram.fnAddressTakenTus.find(sym);
    if (addrIt != wholeProgram.fnAddressTakenTus.end() &&
        !onlyOneTu(addrIt->second))
      return false;
    return true;
  };

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
    // Local array of 1..kMaxOwnerArrayElements elements: the struct_def
    // `Default` derive MVP limit. Scalar and oversized bases keep the
    // Phase-1b lowering.
    if (!arrayType || arrayType->getSize().getZExtValue() == 0 ||
        arrayType->getSize().getZExtValue() > kMaxOwnerArrayElements)
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
    // Stage 1 (owner-index-return extension): pointer-returning methods
    // whose every `return` operand is proven, below, to root in THIS class
    // stage here instead of disqualifying; finalized into
    // `ownerIndexReturns` only once `qualifies` survives every other check
    // for the whole class (mirroring `methodFns`/`methodPlans` below).
    llvm::SmallPtrSet<const clang::FunctionDecl *, 4> pendingIndexReturns;
    for (const clang::FunctionDecl *fn : methodFns) {
      // A pointer return no longer unconditionally disqualifies the
      // function: it qualifies as an owner-index return (a plain i64
      // element-index result) when every return site's operand resolves,
      // through the SAME interprocedural root resolution
      // `forEachDataPointerCallArg` uses for call arguments, to this one
      // class. Anything else (an unresolvable operand, a mixed-class
      // return, a function pointer return, which stays the historical
      // fn-address kind) keeps the historical disqualification.
      bool disqualifyingReturn = false;
      if (isPointerType(fn->getReturnType()) &&
          !isFunctionPointer(fn->getReturnType())) {
        const clang::FunctionDecl *definition = fn->getDefinition();
        SmallVector<const clang::ReturnStmt *> returns;
        PointerRegionAnalysis returnRegions;
        returnRegions.literalTemps = &literalTemps;
        if (definition && definition->hasBody()) {
          collectReturnStmts(definition->getBody(), returns);
          returnRegions.analyze(astContext(), definition->getBody());
        }
        if (returns.empty()) {
          disqualifyingReturn = true;
        } else {
          for (const clang::ReturnStmt *ret : returns) {
            const clang::VarDecl *root =
                ret->getRetValue()
                    ? resolveArgRoot(returnRegions, ret->getRetValue())
                    : nullptr;
            if (!root || unionFind.find(root) != entry.first) {
              disqualifyingReturn = true;
              break;
            }
          }
        }
        if (!disqualifyingReturn)
          pendingIndexReturns.insert(fn);
      }
      // All-or-nothing per function: every data-pointer parameter of the
      // function must resolve into this one class, the return type must be
      // a plain value or a proven owner-index return, the function may not
      // be the owner itself or C `main`, and all of its call sites must be
      // visible — an externally visible function qualifies when this TU is
      // the whole program OR when the whole-program facts prove no other TU
      // references it (W3.3 G3).
      if (fn == owner || fn->getName() == "main" ||
          (fn->isExternallyVisible() && !soleTranslationUnit &&
           !externalFnFullyVisible(fn)) ||
          disqualifyingReturn) {
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
    for (const clang::FunctionDecl *fn : pendingIndexReturns)
      ownerIndexReturns.insert(fn->getCanonicalDecl());
  }
}

//===----------------------------------------------------------------------===//
// Array-member-pointer planning (Stage 2 of the owner-struct
// self-reference extension, design.md FR-30 follow-on)
//===----------------------------------------------------------------------===//

namespace {

/// Collects every `MemberExpr` below `stmt` that designates `field`
/// (compared by canonical declaration), in source order.
void collectFieldMemberExprs(
    const clang::Stmt *stmt, const clang::FieldDecl *field,
    SmallVectorImpl<const clang::MemberExpr *> &out) {
  if (!stmt)
    return;
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(stmt)) {
    if (const auto *hit =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
        hit && hit->getCanonicalDecl() == field->getCanonicalDecl())
      out.push_back(member);
  }
  for (const clang::Stmt *child : stmt->children())
    collectFieldMemberExprs(child, field, out);
}

/// Maps every `MemberExpr` below `stmt` that designates `field` AND is the
/// left-hand side of a simple (`=`) assignment to that assignment's
/// right-hand side, in source order. A compound assignment
/// (`x->field += y`) or any other mutating shape never populates this map
/// — `planArrayMemberPointers` treats any `field` site absent here but
/// present in `collectFieldMemberExprs`'s result as a plain read, which is
/// correct: a compound assignment reads the field too (through the same
/// arrow base), and its unmodeled STORE is caught by the fact that the
/// statement itself is never specially recognized, so the field stays
/// unpromoted only if some OTHER analysis needed to model the store — it
/// doesn't here, because a compound assignment onto a pointer field is
/// already rejected upstream of this pass (pointer fields do not admit
/// arithmetic), so no such site can exist in an accepted program.
void collectFieldAssignRhs(
    const clang::Stmt *stmt, const clang::FieldDecl *field,
    llvm::DenseMap<const clang::MemberExpr *, const clang::Expr *> &out) {
  if (!stmt)
    return;
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt);
      binary && binary->getOpcode() == clang::BO_Assign) {
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(
            binary->getLHS()->IgnoreParens())) {
      if (const auto *hit =
              llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
          hit && hit->getCanonicalDecl() == field->getCanonicalDecl())
        out[member] = binary->getRHS();
    }
  }
  for (const clang::Stmt *child : stmt->children())
    collectFieldAssignRhs(child, field, out);
}

} // namespace

void CImporter::planArrayMemberPointers(
    const clang::TranslationUnitDecl *unit) {
  // Every self-referential data-pointer field of a promoted owner class's
  // element type is a candidate, keyed by field so a field claimed by two
  // DIFFERENT owner classes (ambiguous — ownership isn't unique) is
  // detected and excluded below.
  llvm::DenseMap<const clang::FieldDecl *, const clang::VarDecl *>
      candidateOwner;
  llvm::DenseMap<const clang::FieldDecl *, unsigned> candidateElementCount;
  llvm::SmallPtrSet<const clang::FieldDecl *, 8> ambiguousFields;

  for (const auto &entry : ownerPlans) {
    const clang::VarDecl *ownerArray = entry.first;
    const clang::ConstantArrayType *arrayType =
        astContext().getAsConstantArrayType(ownerArray->getType());
    if (!arrayType) // Defensive; planOwners only promotes array bases.
      continue;
    uint64_t count64 = arrayType->getSize().getZExtValue();
    // Defensive: planOwners's own gate already enforces
    // 1..kMaxOwnerArrayElements before a class ever reaches `ownerPlans`,
    // so this can only re-trigger if that invariant ever changes.
    if (count64 == 0 || count64 > kMaxOwnerArrayElements)
      continue;
    unsigned elementCount = static_cast<unsigned>(count64);
    clang::QualType element = arrayType->getElementType();
    const clang::RecordDecl *record = recordOfType(element);
    if (!record)
      continue;
    for (const clang::FieldDecl *field : record->fields()) {
      if (!isDataPointer(field->getType()))
        continue;
      clang::QualType pointee =
          field->getType().getCanonicalType()->getPointeeType();
      if (!astContext().hasSameUnqualifiedType(pointee, element))
        continue; // Not self-referential.
      auto [it, inserted] = candidateOwner.try_emplace(field, ownerArray);
      if (!inserted && it->second != ownerArray) {
        ambiguousFields.insert(field);
        continue;
      }
      candidateElementCount[field] = elementCount;
    }
  }
  for (const clang::FieldDecl *field : ambiguousFields)
    candidateOwner.erase(field);

  for (const auto &candidate : candidateOwner) {
    const clang::FieldDecl *field = candidate.first;
    const clang::VarDecl *ownerArray = candidate.second;
    bool usable = true;

    for (const clang::FunctionDecl *func :
         collectPassAFunctionDefinitions(unit)) {
      if (!usable)
        break;
      SmallVector<const clang::MemberExpr *> sites;
      collectFieldMemberExprs(func->getBody(), field, sites);
      if (sites.empty())
        continue;
      llvm::DenseMap<const clang::MemberExpr *, const clang::Expr *>
          assignRhs;
      collectFieldAssignRhs(func->getBody(), field, assignRhs);

      PointerRegionAnalysis regions;
      regions.literalTemps = &literalTemps;
      // Stage 4 (B3): a local assigned FROM an array-member field read
      // (`parent = node->parent;`) needs `recordPointerWrite` to join the
      // local into the arrow base's class instead of rejecting it as a
      // non-address value — see `arrayMemberFieldQuery`'s doc. Pass A runs
      // BEFORE any field is proven (this very loop is doing the proving),
      // so the query answers structural candidacy (still-ambiguous fields
      // were already erased from `candidateOwner` above) rather than full
      // `arrayMemberPtrBindings` membership; the per-field `usable` result
      // computed below is what actually gates the real, final binding.
      regions.arrayMemberFieldQuery = [&](const clang::FieldDecl *f) {
        return candidateOwner.contains(f);
      };
      // Stage 5: a local bound from an owner-index-returning call's result
      // (`root1 = uf_find(node1);`) needs `recordPointerWrite` to
      // re-classify it as rooting in the callee's class, exactly like the
      // emission-time wiring (`ImportCFunctions.cpp`, both prologues). Pass
      // A's own `PointerRegionAnalysis` instance never had this wired
      // before Stage 5, so any local bound from such a call was invisible
      // to `resolveArgRoot` here and every site of the field got poisoned
      // program-wide before the real, emission-time proof was ever
      // consulted. `planOwners` (which fully populates `ownerIndexReturns`)
      // always runs before `planArrayMemberPointers`, so the set is
      // complete by this point.
      regions.ownerIndexReturnQuery =
          [&](const clang::FunctionDecl *callee) {
            return ownerIndexReturns.contains(callee->getCanonicalDecl());
          };
      regions.analyze(astContext(), func->getBody());

      for (const clang::MemberExpr *member : sites) {
        // A dot-form access (`s.field`) has no arrow base to root — Pass A
        // only proves the arrow form usable.
        if (!member->isArrow()) {
          usable = false;
          break;
        }
        const clang::VarDecl *root =
            resolveArgRoot(regions, member->getBase());
        if (!isArrayMemberOwnerRoot(root, ownerArray)) {
          usable = false;
          break;
        }
        auto rhsIt = assignRhs.find(member);
        if (rhsIt == assignRhs.end())
          continue; // A plain read: the arrow-base proof above suffices.
        // A write needs the SAME proof of its right-hand side: either a
        // value that itself roots in this class (the promoted cursor
        // value itself, e.g. `x->self = x;`), or — Stage 4, B4 — the
        // right-hand side is ITSELF an array-member field read whose own
        // arrow base roots in this class (`node->parent = parent->parent;`).
        // `resolveArgRoot` has no `MemberExpr` case (it only resolves
        // variable/parameter-rooted pointer expressions), so the field-read
        // shape is recognized directly here instead: it is exactly as
        // legal a source as the cursor value itself, by the read-side
        // proof this very pass runs for every site of a candidate field.
        const clang::VarDecl *rhsRoot =
            resolveArgRoot(regions, rhsIt->second);
        if (isArrayMemberOwnerRoot(rhsRoot, ownerArray))
          continue;
        bool rhsIsFieldRead = false;
        const clang::Expr *rhsStripped = stripTrivia(rhsIt->second);
        if (const auto *rhsCast =
                llvm::dyn_cast<clang::ImplicitCastExpr>(rhsStripped);
            rhsCast && (rhsCast->getCastKind() == clang::CK_LValueToRValue ||
                       rhsCast->getCastKind() == clang::CK_NoOp))
          rhsStripped = stripTrivia(rhsCast->getSubExpr());
        if (const auto *rhsMember =
                llvm::dyn_cast<clang::MemberExpr>(rhsStripped);
            rhsMember && rhsMember->isArrow()) {
          if (const auto *rhsField = llvm::dyn_cast<clang::FieldDecl>(
                  rhsMember->getMemberDecl());
              rhsField && candidateOwner.lookup(rhsField) == ownerArray) {
            const clang::VarDecl *rhsBaseRoot =
                resolveArgRoot(regions, rhsMember->getBase());
            rhsIsFieldRead = isArrayMemberOwnerRoot(rhsBaseRoot, ownerArray);
          }
        }
        if (!rhsIsFieldRead) {
          usable = false;
          break;
        }
      }
    }

    if (usable) {
      ArrayMemberPointerFacts facts;
      facts.ownerArray = ownerArray;
      facts.elementCount = candidateElementCount.lookup(field);
      arrayMemberPtrBindings[field] = facts;
    }
  }
}

//===----------------------------------------------------------------------===//
// Malloc index-handle node-pool planning (W4.2e Part B, FR-39, Pass A)
//===----------------------------------------------------------------------===//

namespace {
/// Returns whether `target` appears anywhere in `parent`'s subtree.
bool stmtContains(const clang::Stmt *parent, const clang::Stmt *target) {
  if (!parent)
    return false;
  if (parent == target)
    return true;
  for (const clang::Stmt *child : parent->children())
    if (stmtContains(child, target))
      return true;
  return false;
}

/// The single self-referential data-pointer field of `record` (a pointer to
/// `record` itself), or null when there is not exactly one.
const clang::FieldDecl *soleSelfRefPointerField(clang::ASTContext &ctx,
                                                const clang::RecordDecl *record) {
  const clang::FieldDecl *found = nullptr;
  for (const clang::FieldDecl *field : record->fields()) {
    if (!isDataPointer(field->getType()))
      continue;
    clang::QualType pointee =
        field->getType().getCanonicalType()->getPointeeType();
    const clang::RecordDecl *pointeeRecord = recordOfType(pointee);
    if (pointeeRecord &&
        pointeeRecord->getCanonicalDecl() == record->getCanonicalDecl()) {
      if (found)
        return nullptr; // More than one self-ref field: not modeled.
      found = field;
      continue;
    }
    return nullptr; // A data-pointer field that is NOT self-ref: not modeled.
  }
  return found;
}
} // namespace

bool CImporter::foldLoopTripCount(PointerRegionAnalysis &regions,
                                  const clang::Stmt *stmt,
                                  uint64_t &out) const {
  // Only the canonical counted `for (i = A; i < B; i++)` loop (or its `<=`
  // variant) folds; anything else leaves the pool capacity undeterminable.
  const auto *forStmt = llvm::dyn_cast<clang::ForStmt>(stmt);
  if (!forStmt || !forStmt->getInit() || !forStmt->getCond() ||
      !forStmt->getInc())
    return false;
  // Increment must be a bare `++i`/`i++` on the induction variable.
  const auto *inc =
      llvm::dyn_cast<clang::UnaryOperator>(forStmt->getInc());
  if (!inc || !inc->isIncrementOp())
    return false;
  const clang::VarDecl *iv = asLocalVarRef(inc->getSubExpr());
  if (!iv)
    return false;
  // Init: `int i = A;` (a DeclStmt for iv) or `i = A;`.
  uint64_t start = 0;
  bool haveStart = false;
  if (const auto *declStmt =
          llvm::dyn_cast<clang::DeclStmt>(forStmt->getInit())) {
    if (declStmt->isSingleDecl())
      if (const auto *var =
              llvm::dyn_cast<clang::VarDecl>(declStmt->getSingleDecl()))
        if (var == iv && var->getInit() &&
            regions.evalFoldableInt(var->getInit(), start))
          haveStart = true;
  } else if (const auto *assign =
                 llvm::dyn_cast<clang::BinaryOperator>(forStmt->getInit())) {
    if (assign->getOpcode() == clang::BO_Assign &&
        asLocalVarRef(assign->getLHS()) == iv &&
        regions.evalFoldableInt(assign->getRHS(), start))
      haveStart = true;
  }
  if (!haveStart)
    return false;
  // Cond: `i < B` or `i <= B` against the induction variable.
  const auto *cond =
      llvm::dyn_cast<clang::BinaryOperator>(forStmt->getCond());
  if (!cond)
    return false;
  clang::BinaryOperatorKind op = cond->getOpcode();
  if (op != clang::BO_LT && op != clang::BO_LE)
    return false;
  if (asLoadedLocalVarRef(cond->getLHS()) != iv)
    return false;
  uint64_t bound = 0;
  if (!regions.evalFoldableInt(cond->getRHS(), bound))
    return false;
  if (op == clang::BO_LE)
    bound += 1;
  if (bound <= start)
    return false;
  out = bound - start;
  return true;
}

void CImporter::planMallocPool(const clang::TranslationUnitDecl *unit) {
  for (const clang::FunctionDecl *func : collectPassAFunctionDefinitions(unit)) {
    const clang::Stmt *body = func->getBody();
    if (!body)
      continue;

    // 1. The pooled record type: every pointer-to-record local whose pointee
    //    has a sole self-ref pointer field must name ONE record T.
    const clang::RecordDecl *poolStruct = nullptr;
    const clang::FieldDecl *nextField = nullptr;
    SmallVector<const clang::VarDecl *, 8> handleVars;
    bool multiType = false;
    std::function<void(const clang::Stmt *)> collectHandles =
        [&](const clang::Stmt *s) {
          if (!s)
            return;
          if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(s))
            for (const clang::Decl *decl : declStmt->decls())
              if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
                if (var->hasLocalStorage() &&
                    !llvm::isa<clang::ParmVarDecl>(var) &&
                    isDataPointer(var->getType())) {
                  clang::QualType pointee =
                      var->getType().getCanonicalType()->getPointeeType();
                  const clang::RecordDecl *record = recordOfType(pointee);
                  if (!record)
                    return;
                  const clang::FieldDecl *self =
                      soleSelfRefPointerField(astContext(), record);
                  if (!self)
                    return;
                  const auto *canonical =
                      llvm::cast<clang::RecordDecl>(record->getCanonicalDecl());
                  if (!poolStruct) {
                    poolStruct = canonical;
                    nextField = self;
                  } else if (poolStruct != canonical) {
                    multiType = true;
                  }
                  handleVars.push_back(var);
                }
          for (const clang::Stmt *child : s->children())
            collectHandles(child);
        };
    collectHandles(body);
    if (!poolStruct || multiType || handleVars.empty())
      continue;

    // 2. Exactly one malloc(sizeof(struct T)) call site, inside a
    //    foldable-trip-count loop -> capacity.
    SmallVector<const clang::CallExpr *> calls;
    collectCallExprs(body, calls);
    const clang::CallExpr *allocSite = nullptr;
    bool multiAlloc = false;
    for (const clang::CallExpr *call : calls) {
      const clang::CallExpr *alloc = asAllocCall(call);
      if (!alloc || alloc->getDirectCallee()->getName() != "malloc")
        continue;
      if (allocSite) {
        multiAlloc = true;
        break;
      }
      allocSite = alloc;
    }
    if (!allocSite || multiAlloc)
      continue;

    // The malloc's enclosing (innermost) counted loop bounds the pool.
    PointerRegionAnalysis regions;
    regions.literalTemps = &literalTemps;
    regions.analyze(astContext(), body);
    regions.primeForFolding(astContext(), body);
    uint64_t cap = 0;
    {
      // The malloc must be governed by EXACTLY ONE loop (of any kind), and
      // that loop must be a foldable counted `for` -- so the pool capacity
      // equals the total number of allocations. A malloc nested inside more
      // than one loop would allocate trip-count-PRODUCT slots, overflowing a
      // single-loop-sized pool at runtime; such a function stays unpromoted
      // (sound: it falls through to the located region rejection). Every
      // loop kind counts (for / while / do-while / ranged-for), not just
      // `for`, so an inner `while` around the malloc is caught too.
      const clang::ForStmt *enclosing = nullptr;
      unsigned enclosingLoopCount = 0;
      std::function<void(const clang::Stmt *)> findLoop =
          [&](const clang::Stmt *s) {
            if (!s)
              return;
            if (llvm::isa<clang::ForStmt, clang::WhileStmt, clang::DoStmt,
                          clang::CXXForRangeStmt>(s) &&
                s != allocSite && stmtContains(s, allocSite)) {
              ++enclosingLoopCount;
              if (const auto *forStmt = llvm::dyn_cast<clang::ForStmt>(s))
                enclosing = forStmt;
            }
            for (const clang::Stmt *child : s->children())
              findLoop(child);
          };
      findLoop(body);
      if (enclosingLoopCount != 1 || !enclosing ||
          !foldLoopTripCount(regions, enclosing, cap))
        continue;
    }
    if (cap == 0 || cap > 65536)
      continue;

    // 3. Validate every use of every handle local is pool-safe. Anything
    //    outside the allowed set (member access, null-check, handle copy,
    //    self-ref field read/write, free arg, the declaration itself)
    //    leaves the function unpromoted -- sound: it falls through to the
    //    existing region-driven rejection, never a miscompile.
    llvm::SmallPtrSet<const clang::VarDecl *, 8> handleSet(handleVars.begin(),
                                                           handleVars.end());
    auto isHandleRef = [&](const clang::Expr *e) -> const clang::VarDecl * {
      const clang::VarDecl *var = asLoadedLocalVarRef(e);
      if (!var)
        var = asLocalVarRef(e);
      return var && handleSet.contains(var) ? var : nullptr;
    };
    // A self-ref `->next` arrow read whose base is a handle.
    auto isNextRead = [&](const clang::Expr *e) -> bool {
      const clang::Expr *s = stripTrivia(e);
      while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(s)) {
        if (cast->getCastKind() != clang::CK_LValueToRValue &&
            cast->getCastKind() != clang::CK_NoOp)
          break;
        s = stripTrivia(cast->getSubExpr());
      }
      const auto *member = llvm::dyn_cast<clang::MemberExpr>(s);
      if (!member || !member->isArrow())
        return false;
      const auto *field =
          llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
      if (!field ||
          field->getCanonicalDecl() != nextField->getCanonicalDecl())
        return false;
      return isHandleRef(member->getBase()) != nullptr;
    };
    bool valid = true;
    std::function<bool(const clang::Expr *)> okBinding =
        [&](const clang::Expr *rhs) -> bool {
      // A handle binding source: malloc site, NULL, another handle, or a
      // self-ref field read.
      const clang::Expr *e = stripTrivia(rhs);
      if (e->isNullPointerConstant(astContext(),
                                   clang::Expr::NPC_NeverValueDependent) !=
          clang::Expr::NPCK_NotNull)
        return true;
      if (asAllocCall(e) &&
          asAllocCall(e)->getDirectCallee()->getName() == "malloc")
        return true;
      if (isHandleRef(e))
        return true;
      return isNextRead(e);
    };
    std::function<void(const clang::Stmt *)> validate =
        [&](const clang::Stmt *s) {
          if (!valid || !s)
            return;
          // Handle declarations: init must be a legal binding source.
          if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(s)) {
            for (const clang::Decl *decl : declStmt->decls())
              if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
                if (handleSet.contains(var) && var->getInit() &&
                    !okBinding(var->getInit()))
                  valid = false;
          }
          // Handle assignments / self-ref field writes.
          if (const auto *bin = llvm::dyn_cast<clang::BinaryOperator>(s);
              bin && bin->getOpcode() == clang::BO_Assign) {
            if (isHandleRef(bin->getLHS())) {
              if (!okBinding(bin->getRHS()))
                valid = false;
            } else if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(
                           bin->getLHS()->IgnoreParens());
                       member && member->isArrow()) {
              const auto *field = llvm::dyn_cast<clang::FieldDecl>(
                  member->getMemberDecl());
              bool isNextWrite =
                  field &&
                  field->getCanonicalDecl() == nextField->getCanonicalDecl() &&
                  isHandleRef(member->getBase());
              if (isNextWrite && !okBinding(bin->getRHS()))
                valid = false;
              // A non-next arrow write on a handle base (e.g. n->val = i) is
              // an ordinary member write; its rhs is a scalar, always fine.
            }
          }
          // `free(handle)` is allowed; any OTHER call taking a handle escapes.
          if (const auto *call = llvm::dyn_cast<clang::CallExpr>(s)) {
            const clang::FunctionDecl *callee = call->getDirectCallee();
            bool isFree = asFreeCall(call) != nullptr;
            for (const clang::Expr *arg : call->arguments())
              if (isHandleRef(arg) && !isFree)
                valid = false;
            (void)callee;
          }
          // A return / address-of / subscript / non-null compare / pointer
          // arithmetic on a handle escapes or is unmodeled.
          if (const auto *ret = llvm::dyn_cast<clang::ReturnStmt>(s))
            if (ret->getRetValue() && isHandleRef(ret->getRetValue()))
              valid = false;
          if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(s)) {
            if (unary->getOpcode() == clang::UO_AddrOf &&
                isHandleRef(unary->getSubExpr()))
              valid = false;
            if (unary->getOpcode() == clang::UO_Deref &&
                isHandleRef(unary->getSubExpr()))
              valid = false; // *handle (whole struct) is not modeled.
          }
          if (const auto *sub = llvm::dyn_cast<clang::ArraySubscriptExpr>(s))
            if (isHandleRef(sub->getBase()))
              valid = false;
          if (const auto *cmp = llvm::dyn_cast<clang::BinaryOperator>(s)) {
            clang::BinaryOperatorKind op = cmp->getOpcode();
            bool isCmp = op == clang::BO_EQ || op == clang::BO_NE ||
                         op == clang::BO_LT || op == clang::BO_GT ||
                         op == clang::BO_LE || op == clang::BO_GE;
            bool isArith = op == clang::BO_Add || op == clang::BO_Sub;
            if (isCmp || isArith) {
              auto handleNonNull = [&](const clang::Expr *e) {
                if (!isHandleRef(e))
                  return false;
                return true;
              };
              // A comparison/arith with a handle operand is unmodeled UNLESS
              // it is a null-check (handle vs a null constant).
              const clang::Expr *l = cmp->getLHS(), *r = cmp->getRHS();
              bool lHandle = handleNonNull(l), rHandle = handleNonNull(r);
              bool otherNull =
                  (lHandle && r->isNullPointerConstant(
                                  astContext(),
                                  clang::Expr::NPC_NeverValueDependent) !=
                                  clang::Expr::NPCK_NotNull) ||
                  (rHandle && l->isNullPointerConstant(
                                  astContext(),
                                  clang::Expr::NPC_NeverValueDependent) !=
                                  clang::Expr::NPCK_NotNull);
              if ((lHandle || rHandle) &&
                  !(isCmp && (op == clang::BO_EQ || op == clang::BO_NE) &&
                    otherNull))
                valid = false;
            }
          }
          for (const clang::Stmt *child : s->children())
            validate(child);
        };
    validate(body);
    if (!valid)
      continue;

    // Promote: record the pool and mark every handle + the next field.
    MallocPoolFacts facts;
    facts.structDecl = poolStruct;
    facts.nextField = nextField;
    facts.cap = static_cast<unsigned>(cap);
    facts.allocSite = allocSite;
    mallocPools[func->getCanonicalDecl()] = facts;
    for (const clang::VarDecl *var : handleVars)
      poolHandleVars[var] = func->getCanonicalDecl();
    poolNextFields.insert(nextField->getCanonicalDecl());
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

void CImporter::collectCellSliceCallFacts(clang::ASTContext &context) {
  astContextPtr = &context;
  const clang::TranslationUnitDecl *unit = context.getTranslationUnitDecl();
  for (const clang::FunctionDecl *caller :
       collectPassAFunctionDefinitions(unit)) {
    SmallVector<const clang::CallExpr *> calls;
    collectCallExprs(caller->getBody(), calls);
    for (const clang::CallExpr *call : calls) {
      const clang::FunctionDecl *callee = call->getDirectCallee();
      // Only externally visible callees need whole-program facts: an
      // internal-linkage function is only callable within its own TU, so its
      // per-TU cell-slice analysis already sees all its call sites.
      if (!callee || callee->isVariadic() || isSystemHeaderDecl(callee) ||
          !callee->isExternallyVisible())
        continue;
      unsigned numParams = callee->getNumParams();
      for (auto [index, argExpr] : llvm::enumerate(call->arguments())) {
        if (index >= numParams)
          break; // A variadic tail (excluded above) has no matching param.
        const clang::ParmVarDecl *param = callee->getParamDecl(index);
        if (!isPointerType(param->getType()) ||
            isFunctionPointer(param->getType()))
          continue;
        std::string key = (mlirFuncName(callee) + "#" + llvm::Twine(index)).str();
        // Ensure the key exists even if its only bindings are internal
        // globals (so a never-externally-based parameter is still a known,
        // eligible key).
        (void)wholeProgram.cellSliceParamExtGlobals[key];
        // A pointer-to-pointer parameter has no cell-slice representation.
        if (param->getType()
                .getCanonicalType()
                ->getPointeeType()
                .getCanonicalType()
                ->isPointerType()) {
          wholeProgram.cellSliceParamPoisoned.insert(key);
          continue;
        }
        const clang::Expr *arg = argExpr;
        if (const clang::VarDecl *global = asDecayedGlobalArrayArg(arg)) {
          // An externally visible global is a shared whole-program base; an
          // internal one stays per-TU (recorded only implicitly via the key's
          // existence above), so the generic parameter can back a different
          // internal global in each TU.
          if (global->isExternallyVisible())
            wholeProgram.cellSliceParamExtGlobals[key].insert(
                globalVarSymbolName(global));
          continue;
        }
        // A directly forwarded pointer parameter (`f(p)`): conservatively
        // poison — resolving the forwarded parameter's own bases across TUs
        // is beyond this wave, and this shape has no cross-TU test; poisoning
        // preserves the historical rejection rather than risk unsoundness.
        // Everything else (a local-array decay, an interior pointer, an
        // arbitrary expression) is a Cell-less argument that poisons the key.
        wholeProgram.cellSliceParamPoisoned.insert(key);
      }
    }
  }
}

void CImporter::finalizeCellSliceWholeProgram() {
  // A parameter key is eligible when it is not poisoned and is backed by at
  // most one externally visible global (the whole-program multi-base guard).
  // A global is eligible when it appears as a cell-slice base AND every
  // parameter it binds to is itself clean (non-poisoned, single external
  // base) — a global reached through any multi-base or poisoned parameter is
  // contaminated.
  llvm::StringSet<> contaminatedGlobals;
  llvm::StringSet<> candidateGlobals;
  for (const auto &entry : wholeProgram.cellSliceParamExtGlobals) {
    llvm::StringRef key = entry.first();
    const llvm::StringSet<> &globals = entry.second;
    bool clean =
        !wholeProgram.cellSliceParamPoisoned.contains(key) && globals.size() <= 1;
    if (clean)
      wholeProgram.cellSliceEligibleParamKeys.insert(key);
    for (const auto &g : globals) {
      if (clean)
        candidateGlobals.insert(g.first());
      else
        contaminatedGlobals.insert(g.first());
    }
  }
  for (const auto &g : candidateGlobals)
    if (!contaminatedGlobals.contains(g.first()))
      wholeProgram.cellSliceEligibleGlobals.insert(g.first());
}

bool CImporter::cellSliceParamEligibleWholeProgram(
    const clang::FunctionDecl *fn, unsigned paramIndex) const {
  std::string key =
      (mlirFuncName(fn) + "#" + llvm::Twine(paramIndex)).str();
  return wholeProgram.cellSliceEligibleParamKeys.contains(key);
}

bool CImporter::cellSliceGlobalEligibleWholeProgram(
    llvm::StringRef symbol) const {
  return wholeProgram.cellSliceEligibleGlobals.contains(symbol);
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

    // In a project import an externally visible function may be called from
    // an unseen TU with a local argument; its class must not turn its
    // parameters into cell-slices. W3.3 G4/G5/G6: the whole-program call
    // facts lift this per-parameter — a parameter whose every project-wide
    // call argument is a qualifying global (single external base, no local)
    // is safe. The per-TU checks below (body escape, null-check, type
    // validity, the global/owner gates) still all fire.
    if (!soleTranslationUnit && func->isExternallyVisible())
      for (auto [index, param] : llvm::enumerate(func->parameters()))
        if (isPointerType(param->getType()) &&
            !isFunctionPointer(param->getType()) &&
            !cellSliceParamEligibleWholeProgram(func, index))
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
          (!soleTranslationUnit && global->isExternallyVisible() &&
           !cellSliceGlobalEligibleWholeProgram(globalVarSymbolName(global)))) {
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
          (!soleTranslationUnit && fn->isExternallyVisible() &&
           !cellSliceParamEligibleWholeProgram(
               fn, param->getFunctionScopeIndex())) ||
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
  // FR-48: binding a variable to a C++ reference PARAMETER takes its
  // address just as surely as `&x` does — the call emits an
  // `emitrust.addr_of` of the variable's place — but C++ spells it with no
  // operator at all, so this walk would otherwise never see it. Without
  // the mark the variable stays a promotable rank-0 memref cell, which has
  // no `!emitrust.lvalue` place for `emitBorrowArgument` to borrow; the
  // mark forces the same `emitrust.variable` place an `&`-taken C local
  // already gets. This is the sole planning-side change references need:
  // the address-taken set is the one fact about a variable that its own
  // declaration cannot supply, because it is a property of how CALLERS
  // use it.
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt))
    if (const clang::FunctionDecl *callee = calleeParamSource(call))
      for (unsigned index = 0,
                    count = std::min<unsigned>(call->getNumArgs(),
                                               callee->getNumParams());
           index < count; ++index)
        if (isCxxReferenceDecl(callee->getParamDecl(index)))
          if (const clang::VarDecl *root = placeExprRoot(call->getArg(index)))
            addressTaken.insert(root);
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
    // FR-53: every rejection this planner can raise is a fact about ONE
    // definition — the escaping parameter and the write-through are both
    // located inside `func`'s own body — so under recovery the definition is
    // credited with it and dropped, and the walk continues. Without recovery
    // the failure propagates and the whole translation unit is rejected,
    // instruction for instruction as before.
    if (recoverFromRejections) {
      if (recoverPlannerRejection(decl,
                                  [&] { return planCursorParamsFor(func); }) ==
          PlannerRecovery::Unattributable)
        return failure();
      continue;
    }
    if (failed(planCursorParamsFor(func)))
      return failure();
  }
  return success();
}

LogicalResult CImporter::planCursorParamsFor(const clang::FunctionDecl *func) {
  // C main's `char **argv` has its own policy (dropped from the
  // imported signature; uses rejected) — never a string cursor.
  if (func->isMain())
    return success();
  SmallVector<const clang::ParmVarDecl *, 2> eligible;
  for (const clang::ParmVarDecl *param : func->parameters())
    if (isCharPointerPointerType(param->getType()))
      eligible.push_back(param);
  if (eligible.empty())
    return success();
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
  llvm::SmallPtrSet<const clang::ParmVarDecl *, 2> candidates(eligible.begin(),
                                                              eligible.end());
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
  // The admissions are the LAST thing this function does, so a rejection
  // above leaves `cursorParams` untouched: there is no partial plan to
  // inherit, and the all-or-nothing rule ("either every eligible parameter
  // of this definition is a cursor, or none is") holds under recovery too.
  for (const clang::ParmVarDecl *param : eligible)
    cursorParams.insert(param);
  return success();
}

LogicalResult
CImporter::planVaMonomorph(const clang::TranslationUnitDecl *unit) {
  if (!recoverFromRejections)
    return planVaMonomorphOnce(unit);
  // FR-53 under recovery: REPLAN from scratch after each attributed
  // rejection instead of continuing with the half-built plan the rejection
  // interrupted. The clone list of a target and the per-site clone indices
  // are built incrementally in the last phase below, so a rejection there
  // leaves both partially populated; and dropping a variadic DEFINITION has
  // to un-enumerate every call site that was planned against it, which no
  // local patch can do. Replanning makes the plan the import finally
  // consults, by construction, the plan a non-recovering run over exactly
  // the surviving declarations would have produced.
  //
  // The plan maps accumulate across the translation units of a project, so
  // a round trip restores them to the state this TU inherited rather than
  // clearing them (which would discard earlier TUs' plans).
  auto plansAtEntry = vaMonomorphPlans;
  auto sitesAtEntry = vaCallSiteClones;
  // Every round records exactly one more declaration in `plannerRejections`
  // and every round skips the ones already there, so the loop is bounded by
  // the number of top-level declarations.
  for (;;) {
    vaMonomorphPlans = plansAtEntry;
    vaCallSiteClones = sitesAtEntry;
    pendingPlannerAttribution = nullptr;
    size_t recordedBefore = plannerRejections.size();
    switch (recoverPlannerRejection(
        /*attribution=*/nullptr, [&] { return planVaMonomorphOnce(unit); })) {
    case PlannerRecovery::Planned:
      return success();
    case PlannerRecovery::Recorded:
      // Termination: a round that records nothing new would repeat forever.
      // It cannot happen — every phase skips the declarations already in the
      // map, so a re-credited declaration would have to be one the round did
      // not look at — but a `for (;;)` deserves the check rather than the
      // argument.
      if (plannerRejections.size() == recordedBefore) {
        vaMonomorphPlans = plansAtEntry;
        vaCallSiteClones = sitesAtEntry;
        return failure();
      }
      continue;
    case PlannerRecovery::Unattributable:
      // No declaration could be credited: propagate, exactly as a
      // non-recovering run would, with the plans left as this TU found them.
      vaMonomorphPlans = plansAtEntry;
      vaCallSiteClones = sitesAtEntry;
      return failure();
    }
  }
}

LogicalResult
CImporter::planVaMonomorphOnce(const clang::TranslationUnitDecl *unit) {
  // Gather this TU's va_list-using variadic definitions.
  SmallVector<const clang::FunctionDecl *, 4> targets;
  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl) ||
        plannerRejections.contains(decl))
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
          // FR-53: a fact about this variadic DEFINITION. Dropping it also
          // drops it from `targets` on the replan, so no call site is
          // enumerated for it and every caller rejects at its own call.
          pendingPlannerAttribution = func;
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
            !consumed.contains(ref)) {
          // FR-53: likewise a fact about this variadic definition.
          pendingPlannerAttribution = func;
          return emitError(translateLoc(ref->getBeginLoc()))
                 << "unsupported: va_list escapes variadic definition";
        }
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
    /// FR-53: the top-level declaration whose body or initializer contains
    /// `call`. The clone assignment below can reject a site for a reason that
    /// is a property of the CALL (too few arguments, an extra this importer
    /// cannot pass by value), which belongs to the declaration that wrote it,
    /// not to the variadic definition it names.
    const clang::Decl *owner;
  };
  SmallVector<SiteRecord, 8> sites;
  llvm::SmallPtrSet<const clang::Expr *, 16> calleeRefs;
  const clang::Decl *scanOwner = nullptr;
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
        sites.push_back({call, target, scanOwner});
        calleeRefs.insert(strippedImplicitRef(call->getCallee()));
      }
    }
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
      if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()))
        if (canonicalTargets.count(fn->getCanonicalDecl()) &&
            !calleeRefs.contains(ref)) {
          // FR-53: taking the address is something the SCANNED declaration
          // does. Dropping it removes the reference and leaves the variadic
          // definition monomorphizable from its remaining call sites.
          pendingPlannerAttribution = scanOwner;
          return emitError(translateLoc(ref->getBeginLoc()))
                 << "unsupported: address of variadic definition";
        }
    for (const clang::Stmt *child : stmt->children())
      if (failed(scan(child)))
        return failure();
    return success();
  };
  for (const clang::Decl *decl : unit->decls()) {
    if (decl->isImplicit() || isSystemHeaderDecl(decl) ||
        plannerRejections.contains(decl))
      continue;
    scanOwner = decl;
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
  scanOwner = nullptr;

  // One clone per distinct extras signature; call sites record their
  // clone index. Every target gets a plan entry — a site-less definition
  // plans zero clones and drops entirely at import.
  for (const clang::FunctionDecl *func : targets)
    (void)vaMonomorphPlans[func->getCanonicalDecl()];
  for (const SiteRecord &site : sites) {
    VaMonomorphPlan &plan =
        vaMonomorphPlans[site.target->getCanonicalDecl()];
    unsigned named = site.target->getNumParams();
    if (site.call->getNumArgs() < named) {
      pendingPlannerAttribution = site.owner;
      return emitError(translateLoc(site.call->getBeginLoc()))
             << "unsupported: call argument count mismatch";
    }
    SmallVector<Type, 4> extraTypes;
    for (unsigned index = named; index < site.call->getNumArgs(); ++index) {
      const clang::Expr *argument = site.call->getArg(index);
      Location argLoc = translateLoc(argument->getBeginLoc());
      // Extras pass BY VALUE (clang has already applied the default
      // argument promotions); a data-pointer extra has no by-value
      // representation under the decomposition.
      if (isDataPointer(argument->getType())) {
        pendingPlannerAttribution = site.owner;
        return emitError(argLoc)
               << "unsupported: pointer argument to a variadic call";
      }
      FailureOr<Type> mapped = mapType(argument->getType(), argLoc);
      if (failed(mapped)) {
        // `mapType` already emitted the located diagnostic; the attribution
        // is all this adds.
        pendingPlannerAttribution = site.owner;
        return failure();
      }
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

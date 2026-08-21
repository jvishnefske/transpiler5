//===- ImportCBorrowBundle.cpp - FR-101 borrow-bundle scalarization ------===//
//
// Part of the EmitRust project.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// FR-101: scalar replacement of BORROW BUNDLES, performed on the clang AST
/// BEFORE any planner or analysis runs.
///
/// The shape is one pervasive C idiom (heatshrink's `output_info`,
/// heatshrink_decoder.c:36-40): a function-LOCAL struct whose members are
/// BORROWS of the enclosing function's own parameters
///
///     typedef struct { uint8_t *buf; size_t buf_size; size_t *output_size; }
///         output_info;
///     ... poll(uint8_t *out_buf, size_t out_buf_size, size_t *output_size) {
///           output_info oi;
///           oi.buf = out_buf; oi.buf_size = out_buf_size;
///           oi.output_size = output_size;
///           st_yield_literal(hsd, &oi);
///
/// built once from those parameters and then passed BY ADDRESS down a chain
/// of helpers that only project its members (`oi->buf[(*oi->output_size)++]`).
/// The CTS-P2 static-binding model resolves a pointer member bound to a
/// sibling local WITHIN one function, but it cannot carry that binding ACROSS
/// a call: the callee has no way to name the caller's target object. Rust's
/// answer would be a lifetime-parametric borrow struct, which is outside the
/// region/cursor value model entirely.
///
/// So the answer is SROA, and it is applied to the C AST rather than to the
/// IR: the local instance and its member-binding stores are erased, each
/// `B *` PARAMETER expands IN PLACE into one parameter per member, and every
/// `&oi` / bare-`oi` argument expands into the member sources. After the
/// rewrite the translation unit is ordinary scalar C, so NOTHING downstream
/// is bundle-aware — the residue lands on machinery that already exists
/// (slice parameters for the byte member, FR-100 callee-aware forwarding for
/// the `size_t *` cursor member). That is why this increment adds no value
/// kind, no dialect operation and no lifetime model, and why a translation
/// unit with no admitted bundle keeps its emission byte-for-byte.
///
/// TWO DESIGN POINTS THAT ARE NOT OBVIOUS, both measured before coding:
///
/// 1. A pointer member is SUBSTITUTED, never re-materialized as a per-member
///    pointer LOCAL. Binding `size_t *q = output_size;` in the caller would
///    make `output_size` appear outside a direct dereference, flipping it to
///    a Slice and rejecting `f(&n)` at every call site of the enclosing
///    function ("the address of a scalar object cannot be passed as a slice
///    parameter"). The member's bound source expression is therefore rebuilt
///    fresh at each expansion site instead.
///
/// 2. A `B *` parameter expands to only the members its callee TRANSITIVELY
///    uses, computed as a call-graph fixpoint over the forwarding edges (the
///    same monotone shape as FR-100's `computeForwardSliceParams`). Expanding
///    to ALL members makes a PANIC reachable on legal C: a callee that never
///    touches the byte member would take a degenerate `&mut u8`, and its
///    caller's reborrow `&mut oi_buf[0]` indexes an EMPTY slice whenever the
///    output window has length zero — exactly heatshrink's
///    `poll(hsd, &out[len], cap - len, &n)` when `len == cap`.
///
/// THE GATE is deliberately whole-TU and conjunctive; every clause that fails
/// leaves the record with today's emission and today's LOCATED rejections
/// (see test/Import/C/borrow-bundle-scalarize-invalid.c). The confining
/// clause — fire only when a `B *` PARAMETER actually exists — is what makes
/// the change additive: a bundle-SHAPED struct that is never passed by
/// address (pointers-member.c's `struct Q`) is never touched.
//
//===----------------------------------------------------------------------===//

#include "CImporterInternal.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"

#include <functional>
#include <string>

namespace {

using llvm::SmallBitVector;

/// The record a `T *` names, or null when `type` is not a pointer to a
/// (complete) struct. Const/volatile pointees are excluded: a `const B *`
/// parameter is outside this wave (heatshrink has none) and admitting one
/// would need a per-member constness decision the gate does not make.
const clang::RecordDecl *pointeeRecordOf(clang::QualType type) {
  clang::QualType canonical = type.getCanonicalType();
  const auto *pointer = canonical->getAs<clang::PointerType>();
  if (!pointer)
    return nullptr;
  clang::QualType pointee = pointer->getPointeeType();
  if (pointee.hasQualifiers())
    return nullptr;
  const auto *recordType = pointee.getCanonicalType()->getAs<clang::RecordType>();
  if (!recordType)
    return nullptr;
  const clang::RecordDecl *decl = recordType->getDecl();
  return decl->getDefinition() ? decl->getDefinition() : decl;
}

/// The record `type` itself names (by value), or null.
const clang::RecordDecl *valueRecordOf(clang::QualType type) {
  const auto *recordType = type.getCanonicalType()->getAs<clang::RecordType>();
  if (!recordType)
    return nullptr;
  const clang::RecordDecl *decl = recordType->getDecl();
  return decl->getDefinition() ? decl->getDefinition() : decl;
}

/// Whether `type` mentions `record` anywhere reachable by peeling pointers,
/// arrays and function types. The gate uses this as its catch-all: any
/// appearance of the bundle type outside the four recognized positions
/// (a function-local instance, a `B *` parameter, a `B *` prototype
/// parameter, and the member projections of one) disqualifies the record.
bool typeMentionsRecord(clang::QualType type, const clang::RecordDecl *record,
                        unsigned depth = 0) {
  if (type.isNull() || depth > 8)
    return false;
  clang::QualType canonical = type.getCanonicalType();
  if (const auto *pointer = canonical->getAs<clang::PointerType>())
    return typeMentionsRecord(pointer->getPointeeType(), record, depth + 1);
  if (const clang::ArrayType *array = canonical->getAsArrayTypeUnsafe())
    return typeMentionsRecord(array->getElementType(), record, depth + 1);
  if (const auto *function = canonical->getAs<clang::FunctionProtoType>()) {
    if (typeMentionsRecord(function->getReturnType(), record, depth + 1))
      return true;
    for (clang::QualType param : function->getParamTypes())
      if (typeMentionsRecord(param, record, depth + 1))
        return true;
    return false;
  }
  if (const auto *function = canonical->getAs<clang::FunctionType>())
    return typeMentionsRecord(function->getReturnType(), record, depth + 1);
  return valueRecordOf(canonical) == record;
}

/// The variable a bare reference names, after peeling parentheses and the
/// implicit lvalue-to-rvalue / no-op conversions clang wraps operands in.
/// Anything else (arithmetic, a subscript, a cast that changes the type)
/// returns null and therefore fails the gate.
const clang::VarDecl *bareVarRef(const clang::Expr *expr) {
  if (!expr)
    return nullptr;
  const clang::Expr *stripped = expr->IgnoreParens();
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(stripped)) {
    if (cast->getCastKind() != clang::CK_LValueToRValue &&
        cast->getCastKind() != clang::CK_NoOp)
      return nullptr;
    stripped = cast->getSubExpr()->IgnoreParens();
  }
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stripped);
  return ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
}

/// Everything the gate learned about one function-local bundle instance.
struct InstanceFacts {
  const clang::VarDecl *var = nullptr;
  const clang::Stmt *declStmt = nullptr;
  int declIndex = -1;
  /// Per member: the declaration whose value the member was bound to, and
  /// the top-level statement that bound it.
  llvm::SmallVector<const clang::VarDecl *, 4> sources;
  llvm::SmallVector<const clang::Stmt *, 4> bindStmts;
  llvm::SmallVector<int, 4> bindIndex;
  /// The lowest top-level statement index at which the instance's address
  /// is passed; every binding must precede it.
  int firstUseIndex = -1;
};

/// Which canonical function and which ORIGINAL parameter index a `B *`
/// parameter occupies. Recorded before the signatures are mutated, because
/// the rewrite has to look the expansion up from a `DeclRefExpr` that still
/// names the (now unlisted) original parameter.
struct BundleParamKey {
  const clang::FunctionDecl *fn = nullptr;
  unsigned index = 0;
};

/// The whole transform for ONE candidate record. Constructed per record and
/// run to completion (gate, fixpoint, rewrite) before the next candidate is
/// considered, so a translation unit with two independent bundle types is
/// handled by two sequential, self-consistent passes.
class BundleScalarizer {
public:
  BundleScalarizer(clang::ASTContext &context, const clang::RecordDecl *record)
      : context(context), record(record) {
    for (const clang::FieldDecl *field : record->fields())
      fields.push_back(field);
  }

  /// Runs the gate and, when every clause holds, the rewrite. Returns
  /// whether the AST was modified.
  bool run(clang::TranslationUnitDecl *unit);

private:
  // ---- gate -------------------------------------------------------------
  bool membersAreScalarizable() const;
  bool surveyDeclarations(clang::TranslationUnitDecl *unit);
  bool walkFunction(clang::FunctionDecl *definition);
  void walk(const clang::Stmt *stmt, bool topLevel);
  bool walkCall(const clang::CallExpr *call);
  bool checkSourcesUnmodified(const clang::Stmt *stmt);

  // ---- fixpoint ---------------------------------------------------------
  void solveUsedMembers();

  // ---- rewrite ----------------------------------------------------------
  void rewriteSignatures();
  void rewriteBodies();
  clang::Stmt *rewriteStmt(clang::Stmt *stmt);
  clang::Stmt *rebuildCall(clang::CallExpr *call);
  clang::Expr *expandedArgument(const clang::Expr *argument, unsigned member);

  // ---- helpers ----------------------------------------------------------
  unsigned fieldIndex(const clang::FieldDecl *field) const {
    for (auto [index, candidate] : llvm::enumerate(fields))
      if (candidate == field)
        return index;
    return fields.size();
  }
  bool isBundleField(const clang::FieldDecl *field) const {
    return field && field->getParent() == record;
  }
  bool functionHasBundleParam(const clang::FunctionDecl *fn) const {
    for (const clang::ParmVarDecl *param : fn->parameters())
      if (pointeeRecordOf(param->getType()) == record)
        return true;
    return false;
  }
  clang::Expr *makeLValueRef(const clang::VarDecl *decl, clang::QualType type,
                             clang::SourceLocation loc);
  clang::Expr *makeRValueRef(const clang::VarDecl *decl,
                             clang::SourceLocation loc);
  void fail() { ok = false; }

  clang::ASTContext &context;
  const clang::RecordDecl *record;
  llvm::SmallVector<const clang::FieldDecl *, 4> fields;

  bool ok = true;
  bool sawBundleParam = false;
  bool sawInstance = false;

  /// Every function DEFINITION in the translation unit, in declaration
  /// order (the rewrite visits all of them; the gate walks all of them).
  llvm::SmallVector<clang::FunctionDecl *, 16> definitions;

  /// Per instance variable, the recognized facts. Keyed translation-unit
  /// wide; instance variables are unique declarations.
  llvm::DenseMap<const clang::VarDecl *, InstanceFacts> instances;
  /// Every `B *` parameter of every declaration, with its owning canonical
  /// function and ORIGINAL index.
  llvm::DenseMap<const clang::ParmVarDecl *, BundleParamKey> bundleParams;
  /// Members demanded at (canonical function, original parameter index).
  llvm::DenseMap<std::pair<const clang::FunctionDecl *, unsigned>,
                 SmallBitVector>
      usedMembers;
  /// Forwarding edges: the caller position inherits the callee position's
  /// demand.
  llvm::SmallVector<std::pair<std::pair<const clang::FunctionDecl *, unsigned>,
                              std::pair<const clang::FunctionDecl *, unsigned>>,
                    8>
      forwardEdges;
  /// The declarations every binding read; none of them may be modified.
  llvm::DenseSet<const clang::VarDecl *> sourceDecls;

  /// The synthesized member parameters, keyed by (canonical function,
  /// original parameter index, member index).
  llvm::DenseMap<std::pair<const clang::FunctionDecl *, unsigned>,
                 llvm::SmallVector<clang::ParmVarDecl *, 4>>
      synthesized;

  // Walk state.
  const clang::FunctionDecl *currentCanonical = nullptr;
  int currentTopIndex = -1;
};

bool BundleScalarizer::membersAreScalarizable() const {
  if (!record->isStruct() || !record->isCompleteDefinition() || fields.empty())
    return false;
  for (const clang::FieldDecl *field : fields) {
    if (field->isBitField() || field->isAnonymousStructOrUnion() ||
        field->getName().empty())
      return false;
    clang::QualType type = field->getType().getCanonicalType();
    if (const auto *pointer = type->getAs<clang::PointerType>()) {
      clang::QualType pointee = pointer->getPointeeType().getCanonicalType();
      // Pointer to an ARITHMETIC type only: `void *` has no pointee to
      // classify from, a struct pointee (which includes the self-referential
      // node families) and a pointer-to-pointer have no scalar parameter
      // spelling, and a `const` pointee is out of scope for this wave.
      if (pointee.hasQualifiers())
        return false;
      if (!pointee->isArithmeticType() || pointee->isEnumeralType() ||
          pointee->isAnyComplexType())
        return false;
      continue;
    }
    if (!type->isArithmeticType() || type->isEnumeralType() ||
        type->isAnyComplexType())
      return false;
  }
  return true;
}

bool BundleScalarizer::surveyDeclarations(clang::TranslationUnitDecl *unit) {
  for (clang::Decl *decl : unit->decls()) {
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
      // A global instance (or an array/pointer to one) has no enclosing
      // parameters to substitute for its members.
      if (typeMentionsRecord(var->getType(), record))
        return false;
      continue;
    }
    if (const auto *nested = llvm::dyn_cast<clang::RecordDecl>(decl)) {
      const clang::RecordDecl *definition = nested->getDefinition();
      if (definition && definition != record)
        for (const clang::FieldDecl *field : definition->fields())
          if (typeMentionsRecord(field->getType(), record))
            return false;
      continue;
    }
    auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (!fn)
      continue;
    if (typeMentionsRecord(fn->getReturnType(), record))
      return false;
    bool hasBundleParam = false;
    for (auto [index, param] : llvm::enumerate(fn->parameters())) {
      if (pointeeRecordOf(param->getType()) == record) {
        hasBundleParam = true;
        bundleParams[param] =
            BundleParamKey{fn->getCanonicalDecl(),
                           static_cast<unsigned>(index)};
        continue;
      }
      // A by-value B, a `B **`, a `B []` or a `const B *` parameter.
      if (typeMentionsRecord(param->getType(), record))
        return false;
    }
    if (hasBundleParam) {
      // A variadic callee has no declared parameter at a trailing argument
      // position (this is one of the two clauses that keeps c-testsuite
      // 00140 on its current path); an externally visible or body-less one
      // would change an ABI this translation unit does not own.
      if (fn->isVariadic() || fn->isExternallyVisible())
        return false;
      const clang::FunctionDecl *definition = fn->getDefinition();
      if (!definition || !definition->hasBody())
        return false;
      sawBundleParam = true;
    }
    if (fn->doesThisDeclarationHaveABody())
      definitions.push_back(fn);
  }
  return sawBundleParam;
}

void BundleScalarizer::walk(const clang::Stmt *stmt, bool topLevel) {
  if (!stmt || !ok)
    return;

  if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
    for (const clang::Decl *decl : declStmt->decls()) {
      if (const auto *nested = llvm::dyn_cast<clang::RecordDecl>(decl)) {
        const clang::RecordDecl *definition = nested->getDefinition();
        if (definition && definition != record)
          for (const clang::FieldDecl *field : definition->fields())
            if (typeMentionsRecord(field->getType(), record))
              return fail();
        continue;
      }
      const auto *var = llvm::dyn_cast<clang::VarDecl>(decl);
      if (!var)
        continue;
      if (valueRecordOf(var->getType()) == record) {
        // The instance: a plain uninitialized local declared at the top
        // level of the function body, so its bindings dominate every use.
        if (!var->hasLocalStorage() || var->hasInit() || !topLevel ||
            !declStmt->isSingleDecl())
          return fail();
        InstanceFacts &facts = instances[var];
        facts.var = var;
        facts.declStmt = declStmt;
        facts.declIndex = currentTopIndex;
        facts.sources.assign(fields.size(), nullptr);
        facts.bindStmts.assign(fields.size(), nullptr);
        facts.bindIndex.assign(fields.size(), -1);
        sawInstance = true;
        continue;
      }
      if (typeMentionsRecord(var->getType(), record))
        return fail();
      walk(var->getInit(), false);
    }
    return;
  }

  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
    if (binary->isAssignmentOp()) {
      const clang::Expr *lhs = binary->getLHS()->IgnoreParens();
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(lhs)) {
        const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
        if (isBundleField(field)) {
          // A STORE through a `B *` PARAMETER. `p->m` is syntactically the
          // admitted projection, but scalarizing it would turn a
          // caller-VISIBLE store into a store to a by-value copy of the
          // caller's argument: the write would be silently lost. Not a
          // clause design.md's prose names; measured into the gate.
          if (member->isArrow() || binary->getOpcode() != clang::BO_Assign)
            return fail();
          const clang::VarDecl *instance = bareVarRef(member->getBase());
          auto it = instance ? instances.find(instance) : instances.end();
          if (it == instances.end() || !topLevel)
            return fail();
          unsigned index = fieldIndex(field);
          if (index >= fields.size() || it->second.sources[index])
            return fail();
          const clang::VarDecl *source = bareVarRef(binary->getRHS());
          if (!source || !source->hasLocalStorage() ||
              instances.count(source) ||
              !context.hasSameUnqualifiedType(source->getType(),
                                              field->getType()))
            return fail();
          it->second.sources[index] = source;
          it->second.bindStmts[index] = stmt;
          it->second.bindIndex[index] = currentTopIndex;
          sourceDecls.insert(source);
          return;
        }
      }
    }
  }

  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt)) {
    if (unary->isIncrementDecrementOp() ||
        unary->getOpcode() == clang::UO_AddrOf) {
      const clang::Expr *sub = unary->getSubExpr()->IgnoreParens();
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(sub))
        if (isBundleField(
                llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl())))
          return fail(); // `p->m++`, `&p->m`: outside the projection subset.
    }
  }

  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
    if (walkCall(call))
      return;
  }

  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(stmt)) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (isBundleField(field)) {
      // The one admitted projection: `p->m` READ on a `B *` parameter of
      // the function being walked. `inst.m` reads are not admitted (the
      // instance is erased, and nothing in the corpus reads one).
      if (!member->isArrow())
        return fail();
      const clang::VarDecl *base = bareVarRef(member->getBase());
      const auto *param = llvm::dyn_cast_or_null<clang::ParmVarDecl>(base);
      auto it = param ? bundleParams.find(param) : bundleParams.end();
      if (it == bundleParams.end() || it->second.fn != currentCanonical)
        return fail();
      unsigned index = fieldIndex(field);
      if (index >= fields.size())
        return fail();
      SmallBitVector &demand = usedMembers[{it->second.fn, it->second.index}];
      demand.resize(fields.size());
      demand.set(index);
      return;
    }
  }

  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt)) {
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
      // Any appearance of an instance or of a `B *` parameter that the
      // recognized forms above did not already consume.
      if (instances.count(var) || pointeeRecordOf(var->getType()) == record)
        return fail();
    } else if (const auto *fn =
                   llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl())) {
      // The address of a bundle-carrying function escaping into a function
      // pointer: its signature is about to change.
      if (functionHasBundleParam(fn))
        return fail();
    }
  }

  if (const auto *sizeOf =
          llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(stmt))
    if (sizeOf->isArgumentType() &&
        typeMentionsRecord(sizeOf->getArgumentType(), record))
      return fail();

  // Catch-all: any expression still typed in terms of the bundle at this
  // point is an appearance the transform does not model.
  if (const auto *expr = llvm::dyn_cast<clang::Expr>(stmt))
    if (typeMentionsRecord(expr->getType(), record))
      return fail();

  for (const clang::Stmt *child : stmt->children())
    walk(child, false);
}

/// Returns whether `call` was fully consumed here (a call carrying bundle
/// arguments), so the generic descent must not re-walk it.
bool BundleScalarizer::walkCall(const clang::CallExpr *call) {
  const clang::FunctionDecl *callee = call->getDirectCallee();
  if (!callee || !functionHasBundleParam(callee))
    return false;
  if (call->getNumArgs() != callee->getNumParams()) {
    fail();
    return true;
  }
  const clang::FunctionDecl *calleeCanonical = callee->getCanonicalDecl();
  for (auto [index, argument] : llvm::enumerate(call->arguments())) {
    const clang::ParmVarDecl *formal = callee->getParamDecl(index);
    if (pointeeRecordOf(formal->getType()) != record) {
      walk(argument, false);
      continue;
    }
    const clang::Expr *stripped = argument->IgnoreParens();
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stripped);
        unary && unary->getOpcode() == clang::UO_AddrOf) {
      // `&instance`: the consuming position the gate admits.
      const clang::VarDecl *instance = bareVarRef(unary->getSubExpr());
      auto it = instance ? instances.find(instance) : instances.end();
      if (it == instances.end()) {
        fail();
        return true;
      }
      if (it->second.firstUseIndex < 0 ||
          currentTopIndex < it->second.firstUseIndex)
        it->second.firstUseIndex = currentTopIndex;
      continue;
    }
    // A bare forwarded `B *` parameter of the function being walked: the
    // demand of the callee position propagates backward to it.
    const clang::VarDecl *base = bareVarRef(stripped);
    const auto *param = llvm::dyn_cast_or_null<clang::ParmVarDecl>(base);
    auto it = param ? bundleParams.find(param) : bundleParams.end();
    if (it == bundleParams.end() || it->second.fn != currentCanonical) {
      fail();
      return true;
    }
    forwardEdges.push_back({{it->second.fn, it->second.index},
                            {calleeCanonical, static_cast<unsigned>(index)}});
  }
  return true;
}

/// A second, tiny pass: no declaration a member was bound to may be
/// reassigned or have its address taken anywhere in the translation unit,
/// because the rewrite substitutes its VALUE at every expansion site.
bool BundleScalarizer::checkSourcesUnmodified(const clang::Stmt *stmt) {
  if (!stmt)
    return true;
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt))
    if (binary->isAssignmentOp())
      if (const clang::VarDecl *target = bareVarRef(binary->getLHS()))
        if (sourceDecls.count(target))
          return false;
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt))
    if (unary->isIncrementDecrementOp() ||
        unary->getOpcode() == clang::UO_AddrOf)
      if (const clang::VarDecl *target = bareVarRef(unary->getSubExpr()))
        if (sourceDecls.count(target))
          return false;
  for (const clang::Stmt *child : stmt->children())
    if (!checkSourcesUnmodified(child))
      return false;
  return true;
}

bool BundleScalarizer::walkFunction(clang::FunctionDecl *definition) {
  currentCanonical = definition->getCanonicalDecl();
  const auto *body = llvm::dyn_cast<clang::CompoundStmt>(definition->getBody());
  if (!body) {
    // A bundle-carrying function whose body is not a compound statement
    // cannot host the top-level binding block the gate requires.
    return !functionHasBundleParam(definition);
  }
  // Seed the demand map for every `B *` parameter of this definition, so a
  // parameter that is only forwarded still has an entry to propagate into.
  for (auto [index, param] : llvm::enumerate(definition->parameters()))
    if (pointeeRecordOf(param->getType()) == record)
      usedMembers[{currentCanonical, static_cast<unsigned>(index)}].resize(
          fields.size());
  int index = 0;
  for (const clang::Stmt *child : body->body()) {
    currentTopIndex = index++;
    walk(child, /*topLevel=*/true);
    if (!ok)
      return false;
  }
  currentTopIndex = -1;
  return ok;
}

void BundleScalarizer::solveUsedMembers() {
  // Every endpoint gets an entry up front, so the iteration below never
  // inserts into the map (an insertion could rehash and invalidate a
  // reference held across the merge).
  for (const auto &[caller, callee] : forwardEdges) {
    usedMembers[caller].resize(fields.size());
    usedMembers[callee].resize(fields.size());
  }
  for (auto &entry : usedMembers)
    entry.second.resize(fields.size());
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[caller, callee] : forwardEdges) {
      SmallBitVector from = usedMembers.find(callee)->second;
      SmallBitVector &to = usedMembers.find(caller)->second;
      SmallBitVector merged = to;
      merged |= from;
      if (merged != to) {
        to = merged;
        changed = true;
      }
    }
  }
}

clang::Expr *BundleScalarizer::makeLValueRef(const clang::VarDecl *decl,
                                             clang::QualType type,
                                             clang::SourceLocation loc) {
  return new (context) clang::DeclRefExpr(
      context, const_cast<clang::VarDecl *>(decl),
      /*RefersToEnclosingVariableOrCapture=*/false, type, clang::VK_LValue,
      loc);
}

clang::Expr *BundleScalarizer::makeRValueRef(const clang::VarDecl *decl,
                                             clang::SourceLocation loc) {
  clang::QualType type = decl->getType();
  clang::Expr *ref = makeLValueRef(decl, type, loc);
  return clang::ImplicitCastExpr::Create(
      context, type.getUnqualifiedType(), clang::CK_LValueToRValue, ref,
      /*BasePath=*/nullptr, clang::VK_PRValue, clang::FPOptionsOverride());
}

void BundleScalarizer::rewriteSignatures() {
  // One synthesized parameter per demanded member, created once per
  // (canonical function, original parameter index) and SHARED by every
  // redeclaration so a prototype and its definition build one signature.
  llvm::DenseSet<const clang::FunctionDecl *> seen;
  for (clang::FunctionDecl *definition : definitions) {
    if (!functionHasBundleParam(definition))
      continue;
    const clang::FunctionDecl *canonical = definition->getCanonicalDecl();
    if (!seen.insert(canonical).second)
      continue;
    // Every spelling already in the function, so a synthesized name can
    // never capture a local (`oi_buf` beside a member named `buf`).
    llvm::StringSet<> taken;
    for (const clang::ParmVarDecl *param : definition->parameters())
      if (!param->getName().empty())
        taken.insert(param->getName());
    std::function<void(const clang::Stmt *)> collect =
        [&](const clang::Stmt *stmt) {
          if (!stmt)
            return;
          if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt))
            for (const clang::Decl *decl : declStmt->decls())
              if (const auto *named = llvm::dyn_cast<clang::NamedDecl>(decl))
                if (!named->getName().empty())
                  taken.insert(named->getName());
          for (const clang::Stmt *child : stmt->children())
            collect(child);
        };
    collect(definition->getBody());
    for (auto [index, param] : llvm::enumerate(definition->parameters())) {
      if (pointeeRecordOf(param->getType()) != record)
        continue;
      SmallBitVector &demand =
          usedMembers[{canonical, static_cast<unsigned>(index)}];
      demand.resize(fields.size());
      llvm::SmallVector<clang::ParmVarDecl *, 4> slot(fields.size(), nullptr);
      for (unsigned member = 0; member < fields.size(); ++member) {
        if (!demand.test(member))
          continue;
        std::string base = param->getName().empty()
                               ? ("bundle" + llvm::Twine(index)).str()
                               : param->getName().str();
        std::string name = base + "_" + fields[member]->getName().str();
        while (!taken.insert(name).second)
          name += "_";
        clang::QualType type = fields[member]->getType();
        slot[member] = clang::ParmVarDecl::Create(
            context, definition, param->getBeginLoc(), param->getLocation(),
            &context.Idents.get(name), type,
            context.getTrivialTypeSourceInfo(type), clang::SC_None,
            /*DefArg=*/nullptr);
      }
      synthesized[{canonical, static_cast<unsigned>(index)}] = slot;
    }
  }

  // Now rewrite every redeclaration's parameter list and type in place,
  // once per canonical function (`synthesized` is keyed per PARAMETER, and
  // a function may carry more than one bundle parameter).
  llvm::SmallVector<const clang::FunctionDecl *, 8> rewritten;
  llvm::DenseSet<const clang::FunctionDecl *> rewrittenSet;
  for (const auto &entry : synthesized)
    if (rewrittenSet.insert(entry.first.first).second)
      rewritten.push_back(entry.first.first);
  for (const clang::FunctionDecl *target : rewritten) {
    for (clang::FunctionDecl *redecl :
         const_cast<clang::FunctionDecl *>(target)->redecls()) {
      if (!functionHasBundleParam(redecl))
        continue;
      llvm::SmallVector<clang::ParmVarDecl *, 8> params;
      for (auto [index, param] : llvm::enumerate(redecl->parameters())) {
        if (pointeeRecordOf(param->getType()) != record) {
          params.push_back(param);
          continue;
        }
        auto it =
            synthesized.find({target, static_cast<unsigned>(index)});
        if (it == synthesized.end())
          continue;
        for (clang::ParmVarDecl *synth : it->second)
          if (synth)
            params.push_back(synth);
      }
      llvm::SmallVector<clang::QualType, 8> types;
      for (clang::ParmVarDecl *param : params)
        types.push_back(param->getType());
      clang::FunctionProtoType::ExtProtoInfo info;
      if (const auto *proto = redecl->getType()->getAs<clang::FunctionProtoType>())
        info = proto->getExtProtoInfo();
      redecl->setType(context.getFunctionType(redecl->getReturnType(), types,
                                              info));
      redecl->setParams(params);
      for (auto [index, param] : llvm::enumerate(params))
        param->setScopeInfo(0, static_cast<unsigned>(index));
    }
  }
}

clang::Expr *BundleScalarizer::expandedArgument(const clang::Expr *argument,
                                                unsigned member) {
  const clang::Expr *stripped = argument->IgnoreParens();
  clang::SourceLocation loc = argument->getBeginLoc();
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stripped);
      unary && unary->getOpcode() == clang::UO_AddrOf) {
    const clang::VarDecl *instance = bareVarRef(unary->getSubExpr());
    const InstanceFacts &facts = instances.find(instance)->second;
    return makeRValueRef(facts.sources[member], loc);
  }
  const auto *param =
      llvm::cast<clang::ParmVarDecl>(bareVarRef(stripped));
  const BundleParamKey &key = bundleParams.find(param)->second;
  clang::ParmVarDecl *synth =
      synthesized.find({key.fn, key.index})->second[member];
  return makeRValueRef(synth, loc);
}

clang::Stmt *BundleScalarizer::rebuildCall(clang::CallExpr *call) {
  clang::FunctionDecl *callee =
      const_cast<clang::FunctionDecl *>(call->getDirectCallee());
  const clang::FunctionDecl *canonical = callee->getCanonicalDecl();
  llvm::SmallVector<clang::Expr *, 8> args;
  for (unsigned index = 0; index < call->getNumArgs(); ++index) {
    auto it = synthesized.find({canonical, index});
    if (it == synthesized.end()) {
      args.push_back(call->getArg(index));
      continue;
    }
    for (unsigned member = 0; member < fields.size(); ++member)
      if (it->second[member])
        args.push_back(expandedArgument(call->getArg(index), member));
  }
  // The callee reference is rebuilt so its type matches the rewritten
  // signature; a stale function type on the decay cast would be a lie the
  // importer is entitled to believe.
  clang::QualType fnType = callee->getType();
  clang::Expr *fnRef = new (context) clang::DeclRefExpr(
      context, callee, /*RefersToEnclosingVariableOrCapture=*/false, fnType,
      clang::VK_LValue, call->getBeginLoc());
  clang::Expr *decayed = clang::ImplicitCastExpr::Create(
      context, context.getPointerType(fnType),
      clang::CK_FunctionToPointerDecay, fnRef, /*BasePath=*/nullptr,
      clang::VK_PRValue, clang::FPOptionsOverride());
  return clang::CallExpr::Create(context, decayed, args, call->getType(),
                                 call->getValueKind(), call->getRParenLoc(),
                                 clang::FPOptionsOverride());
}

clang::Stmt *BundleScalarizer::rewriteStmt(clang::Stmt *stmt) {
  if (!stmt)
    return stmt;
  for (clang::Stmt *&child : stmt->children())
    child = rewriteStmt(child);
  if (auto *member = llvm::dyn_cast<clang::MemberExpr>(stmt)) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (isBundleField(field) && member->isArrow()) {
      const auto *param = llvm::dyn_cast_or_null<clang::ParmVarDecl>(
          bareVarRef(member->getBase()));
      auto it = param ? bundleParams.find(param) : bundleParams.end();
      if (it != bundleParams.end()) {
        clang::ParmVarDecl *synth =
            synthesized.find({it->second.fn, it->second.index})
                ->second[fieldIndex(field)];
        return makeLValueRef(synth, member->getType(), member->getExprLoc());
      }
    }
  }
  if (auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
    const clang::FunctionDecl *callee = call->getDirectCallee();
    if (callee) {
      const clang::FunctionDecl *canonical = callee->getCanonicalDecl();
      for (unsigned index = 0; index < call->getNumArgs(); ++index)
        if (synthesized.count({canonical, index}))
          return rebuildCall(call);
    }
  }
  return stmt;
}

void BundleScalarizer::rewriteBodies() {
  for (clang::FunctionDecl *definition : definitions) {
    auto *body = llvm::dyn_cast<clang::CompoundStmt>(definition->getBody());
    if (!body)
      continue;
    // Erase the instance declarations and their member-binding stores; a
    // null statement keeps the body's shape without emitting anything.
    for (clang::Stmt *&child : body->body()) {
      bool erase = false;
      for (const auto &[var, facts] : instances) {
        if (facts.declStmt == child) {
          erase = true;
          break;
        }
        if (llvm::is_contained(facts.bindStmts, child)) {
          erase = true;
          break;
        }
      }
      if (erase)
        child = new (context) clang::NullStmt(child->getBeginLoc());
    }
    for (clang::Stmt *&child : body->body())
      child = rewriteStmt(child);
  }
}

bool BundleScalarizer::run(clang::TranslationUnitDecl *unit) {
  if (!membersAreScalarizable())
    return false;
  if (!surveyDeclarations(unit))
    return false;
  for (clang::FunctionDecl *definition : definitions)
    if (!walkFunction(definition))
      return false;
  // Defence in depth: at least one function-LOCAL instance must exist. A
  // `B *` parameter with no instance anywhere (void-param-invalid.c's FIELD
  // arm, multi-tu-external-requirement-const-struct-negative.c's ADDR-STORE
  // arm) keeps its current rejection.
  if (!sawInstance)
    return false;
  for (const auto &[var, facts] : instances) {
    // Every member bound exactly once, every binding dominating every use.
    for (unsigned member = 0; member < fields.size(); ++member) {
      if (!facts.sources[member])
        return false;
      if (facts.bindIndex[member] <= facts.declIndex)
        return false;
      if (facts.firstUseIndex >= 0 &&
          facts.bindIndex[member] >= facts.firstUseIndex)
        return false;
    }
    if (facts.firstUseIndex < 0)
      return false; // Built but never passed: the confining clause.
  }
  for (clang::FunctionDecl *definition : definitions)
    if (!checkSourcesUnmodified(definition->getBody()))
      return false;
  solveUsedMembers();
  rewriteSignatures();
  rewriteBodies();
  return true;
}

} // namespace

void scalarizeBorrowBundles(clang::ASTContext &context) {
  if (context.getLangOpts().CPlusPlus)
    return; // C path only (FR-101 scope).
  clang::TranslationUnitDecl *unit = context.getTranslationUnitDecl();
  // Candidates are the records that appear as a `B *` PARAMETER somewhere in
  // the translation unit. That is the confining clause of the whole FR: a
  // bundle-SHAPED struct that is never passed by address is never even
  // considered, so its emission cannot move by a byte.
  llvm::SmallVector<const clang::RecordDecl *, 4> candidates;
  llvm::DenseSet<const clang::RecordDecl *> seen;
  for (const clang::Decl *decl : unit->decls()) {
    const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (!fn)
      continue;
    for (const clang::ParmVarDecl *param : fn->parameters())
      if (const clang::RecordDecl *pointee = pointeeRecordOf(param->getType()))
        if (seen.insert(pointee).second)
          candidates.push_back(pointee);
  }
  for (const clang::RecordDecl *candidate : candidates)
    BundleScalarizer(context, candidate).run(unit);
}

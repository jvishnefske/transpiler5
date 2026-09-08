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

// W2.17: `checkDropLocalScope` walks OUTWARD from a declaration through its
// enclosing statements, which is the one place this importer needs clang's
// parent map (every other walk is top-down).
#include "clang/AST/ASTTypeTraits.h"
#include "llvm/Support/SaveAndRestore.h"
#include "clang/AST/ParentMapContext.h"

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
  // W2.24: try/catch — only when the TU-level throw plan is active, so a
  // throw-free try keeps the historical generic statement fallback below
  // (pinned in test/Import/Cpp/exceptions-invalid.cpp).
  if (const auto *tryStmt = llvm::dyn_cast<clang::CXXTryStmt>(stmt);
      tryStmt && throwsPlanActive)
    return emitTryStmt(tryStmt);
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


/// W2.17: whether `stmt` contains a `goto`, an indirect goto, or a label.
/// Such a function has its locals HOISTED to function top by the importer's
/// label lowering, so a destructor-carrying local would construct on paths
/// C++ never constructs on (measured: an extra `dtor 0`).
static bool containsGotoOrLabel(const clang::Stmt *stmt) {
  if (!stmt)
    return false;
  if (llvm::isa<clang::GotoStmt, clang::IndirectGotoStmt, clang::LabelStmt,
                clang::AddrLabelExpr>(stmt))
    return true;
  for (const clang::Stmt *child : stmt->children())
    if (containsGotoOrLabel(child))
      return true;
  return false;
}

/// W2.17: whether `expr` can have an observable side effect of the kind a
/// destructor's printf competes with -- i.e. contains a call. Used on a
/// `for` INCREMENT: C++ destroys the body's locals BEFORE evaluating it,
/// while the importer's for -> while lowering renders it at the BOTTOM OF
/// THE BODY, ahead of the drop (measured: `inc`/`dtor` lines swap).
static bool containsCall(const clang::Stmt *stmt) {
  if (!stmt)
    return false;
  if (llvm::isa<clang::CallExpr, clang::CXXConstructExpr>(stmt))
    return true;
  for (const clang::Stmt *child : stmt->children())
    if (containsCall(child))
      return true;
  return false;
}

/// The single `CompoundStmt` `node` is a direct child of, or null.
static const clang::CompoundStmt *
enclosingBlock(clang::ASTContext &context, const clang::Stmt *node) {
  for (const clang::DynTypedNode &parent : context.getParents(*node))
    if (const auto *block = parent.get<clang::CompoundStmt>())
      return block;
  return nullptr;
}

/// FR-193 item 6 / W2.17: the LOOP-EXIT half of the drop-order fence.
///
/// `lift-cf-to-scf` structurizes the CFG. A block that is lexically inside a
/// loop body but from which EVERY path leaves the loop is not on the cycle,
/// so the lift places it AFTER the loop and dispatches it on an exit index.
/// For C that is unobservable. For a loop body holding a destructor-carrying
/// local it is a SILENT MISCOMPILE, because the local's Rust `Drop` runs at
/// the end of the emitted loop-body region -- so a side effect C++ runs
/// BEFORE the destructor is emitted AFTER it. Measured on
/// `for (..) { T a(i); printf("body"); if (i==1) { printf("before break");
/// break; } }`:
///
///   native : body 0 / dtor 0 / body 1 / before break / dtor 1 / after loop
///   emitted: body 0 / dtor 0 / body 1 / dtor 1 / before break / after loop
///
/// `leavesLoop` answers "control always leaves the loop after this
/// statement" in TWO conservative directions, because the fence needs both
/// and they must be wrong in opposite directions:
///
///   * OVER-approximating (`couldAlwaysLeaveLoop`) drives the REJECTION, so
///     being wrong costs a spurious rejection, never a miscompile;
///   * UNDER-approximating (`mustLeaveLoop`) drives the opposite decision --
///     "the body always leaves, so the loop has NO back edge, so there is no
///     cycle for anything to be relocated out of". An unconditional `return`
///     in a loop body is byte-identical today and must stay accepted, so this
///     one may only claim a loop that certainly runs its body (a `do`) or a
///     branch that certainly leaves.
///
/// `swallowers` counts the constructs between the statement and the loop in
/// question that CATCH a `break` -- an inner loop or a `switch`. A `break`
/// leaves the loop only at zero: a `switch` case's `break` inside a loop body
/// and an inner loop's `break` are both byte-identical today.
static bool leavesLoop(const clang::Stmt *stmt, unsigned swallowers,
                       bool overApproximate) {
  if (!stmt)
    return false;
  if (llvm::isa<clang::ReturnStmt>(stmt))
    return true;
  if (llvm::isa<clang::BreakStmt>(stmt))
    return swallowers == 0;
  if (llvm::isa<clang::ContinueStmt>(stmt))
    return false; // the latch IS the cycle: nothing is relocated
  if (llvm::isa<clang::GotoStmt, clang::IndirectGotoStmt>(stmt))
    // A `goto` may jump back INTO the loop, so only the over-approximation
    // may claim it leaves. The whole shape is already rejected anyway: a
    // function carrying a label hoists its locals to function top (see
    // `containsGotoOrLabel`).
    return overApproximate;
  if (const auto *compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
    for (const clang::Stmt *child : compound->body())
      if (leavesLoop(child, swallowers, overApproximate))
        return true;
    return false;
  }
  if (const auto *ifStmt = llvm::dyn_cast<clang::IfStmt>(stmt))
    return ifStmt->getElse() &&
           leavesLoop(ifStmt->getThen(), swallowers, overApproximate) &&
           leavesLoop(ifStmt->getElse(), swallowers, overApproximate);
  // A label wrapper is transparent -- but it must be unwrapped by NAME, not
  // by first child: a `CaseStmt`'s first child is its label EXPRESSION, so
  // `child_begin()` would hand back `1` for `case 1: return i;` and answer
  // "does not leave" for a case that always returns.
  if (const auto *switchCase = llvm::dyn_cast<clang::SwitchCase>(stmt))
    return leavesLoop(switchCase->getSubStmt(), swallowers, overApproximate);
  if (const auto *label = llvm::dyn_cast<clang::LabelStmt>(stmt))
    return leavesLoop(label->getSubStmt(), swallowers, overApproximate);
  if (const auto *attributed = llvm::dyn_cast<clang::AttributedStmt>(stmt))
    return leavesLoop(attributed->getSubStmt(), swallowers, overApproximate);
  if (const auto *doStmt = llvm::dyn_cast<clang::DoStmt>(stmt))
    // A do-while runs its body at least once, so both directions agree.
    return leavesLoop(doStmt->getBody(), swallowers + 1, overApproximate);
  if (llvm::isa<clang::ForStmt, clang::WhileStmt, clang::CXXForRangeStmt,
                clang::SwitchStmt>(stmt)) {
    // Entry is conditional, so "the body always leaves" does NOT prove the
    // statement always leaves; only the over-approximation may say so.
    if (!overApproximate)
      return false;
    const clang::Stmt *body = nullptr;
    if (const auto *forStmt = llvm::dyn_cast<clang::ForStmt>(stmt))
      body = forStmt->getBody();
    else if (const auto *whileStmt = llvm::dyn_cast<clang::WhileStmt>(stmt))
      body = whileStmt->getBody();
    else if (const auto *rangeStmt =
                 llvm::dyn_cast<clang::CXXForRangeStmt>(stmt))
      body = rangeStmt->getBody();
    else
      body = llvm::cast<clang::SwitchStmt>(stmt)->getBody();
    return leavesLoop(body, swallowers + 1, overApproximate);
  }
  return false;
}

static bool couldAlwaysLeaveLoop(const clang::Stmt *stmt,
                                 unsigned swallowers) {
  return leavesLoop(stmt, swallowers, /*overApproximate=*/true);
}

static bool mustLeaveLoop(const clang::Stmt *stmt, unsigned swallowers) {
  return leavesLoop(stmt, swallowers, /*overApproximate=*/false);
}

/// Whether `target` names storage a destructor could read -- anything that is
/// not a plain automatic local. A global store with NO call at all is a
/// measured channel of this defect: `~T() { printf("%d", g_x); }` against
/// `for (..) { T a(i); if (i==1) { g_x = 7; break; } }` prints the PRE-store
/// value, so the `for`-increment gate's call-only screen is not enough here.
static bool writesOutsideAutomaticLocal(const clang::Expr *target) {
  if (!target)
    return false;
  const clang::Expr *bare = target->IgnoreParenImpCasts();
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(bare))
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
      return !var->hasLocalStorage() || var->getType()->isReferenceType();
  return true; // a field, a subscript, a dereference: assume observable
}

/// The first observable side effect anywhere under `stmt`, or null.
static const clang::Stmt *observableEffect(const clang::Stmt *stmt) {
  if (!stmt)
    return nullptr;
  if (llvm::isa<clang::CallExpr, clang::CXXConstructExpr, clang::CXXNewExpr,
                clang::CXXDeleteExpr>(stmt))
    return stmt;
  if (const auto *bin = llvm::dyn_cast<clang::BinaryOperator>(stmt))
    if (bin->isAssignmentOp() && writesOutsideAutomaticLocal(bin->getLHS()))
      return stmt;
  if (const auto *un = llvm::dyn_cast<clang::UnaryOperator>(stmt))
    if (un->isIncrementDecrementOp() &&
        writesOutsideAutomaticLocal(un->getSubExpr()))
      return stmt;
  for (const clang::Stmt *child : stmt->children())
    if (const clang::Stmt *found = observableEffect(child))
      return found;
  return nullptr;
}

/// The first observable effect under `stmt` that the lift relocates PAST the
/// enclosing loop -- one sitting on a path that only ever leaves it.
/// `exitOnly` says the caller already proved that of `stmt` itself.
static const clang::Stmt *relocatedEffect(const clang::Stmt *stmt,
                                          unsigned swallowers, bool exitOnly) {
  if (!stmt)
    return nullptr;
  if (exitOnly)
    return observableEffect(stmt);
  if (const auto *compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
    llvm::SmallVector<const clang::Stmt *, 8> children(compound->body());
    for (size_t i = 0; i < children.size(); ++i) {
      bool childExits = false;
      for (size_t j = i + 1; j < children.size() && !childExits; ++j)
        childExits = couldAlwaysLeaveLoop(children[j], swallowers);
      if (const clang::Stmt *found =
              relocatedEffect(children[i], swallowers, childExits))
        return found;
    }
    return nullptr;
  }
  if (const auto *ifStmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
    // The condition is evaluated BEFORE the decision, so its block still
    // reaches the latch and is not relocated (measured byte-identical).
    if (const clang::Stmt *found =
            relocatedEffect(ifStmt->getCond(), swallowers, false))
      return found;
    for (const clang::Stmt *branch : {ifStmt->getThen(), ifStmt->getElse()})
      if (const clang::Stmt *found = relocatedEffect(
              branch, swallowers, couldAlwaysLeaveLoop(branch, swallowers)))
        return found;
    return nullptr;
  }
  if (const auto *ret = llvm::dyn_cast<clang::ReturnStmt>(stmt))
    // C++ evaluates the operand and THEN destroys the locals; the emitted
    // code carries the operand out with the rest of the exit path (measured:
    // `return side(i)` prints AFTER the drop).
    return relocatedEffect(ret->getRetValue(), swallowers, true);
  if (llvm::isa<clang::ForStmt, clang::WhileStmt, clang::DoStmt,
                clang::CXXForRangeStmt, clang::SwitchStmt>(stmt)) {
    for (const clang::Stmt *child : stmt->children())
      if (const clang::Stmt *found =
              relocatedEffect(child, swallowers + 1, false))
        return found;
    return nullptr;
  }
  for (const clang::Stmt *child : stmt->children())
    if (const clang::Stmt *found = relocatedEffect(child, swallowers, false))
      return found;
  return nullptr;
}

/// One level of the scope chain `checkDropLocalScope` walks outward, recorded
/// so the loop-exit fence can walk back IN.
struct DropScopeStep {
  const clang::CompoundStmt *block; ///< the block `node` sits directly in
  const clang::Stmt *node;          ///< the child of `block` we came from
  bool ownerIsLoop;                 ///< `block` is that loop's body
};

/// FR-193 item 6: the offending side effect, or null if this declaration's
/// drop order survives the lift.
///
/// `steps[0].block` is the declaration's own block; the chain grows outward.
/// Two questions decide the answer:
///
///  1. WHICH loop relocates? The innermost enclosing one whose body does not
///     always leave -- a body that always leaves has no back edge, so it is
///     not a cycle and nothing can be placed "after" it.
///  2. Does the DECLARATION travel with the relocated code? If the
///     declaration itself already sits on an exit-only path, its `let` and
///     everything after it move together and the internal order is preserved
///     (measured byte-identical). Only effects that leave the loop while the
///     declaration stays behind cross the drop.
static const clang::Stmt *
relocatedExitEffect(llvm::ArrayRef<DropScopeStep> steps,
                    const clang::DeclStmt *declStmt) {
  // (1) the innermost enclosing loop that is really a cycle.
  size_t target = steps.size();
  unsigned swallowersAtDecl = 0;
  for (size_t i = 0; i < steps.size(); ++i) {
    if (!steps[i].ownerIsLoop)
      continue;
    if (mustLeaveLoop(steps[i].block, 0)) {
      // No back edge -- but it still catches a `break`.
      ++swallowersAtDecl;
      continue;
    }
    target = i;
    break;
  }
  if (target == steps.size())
    return nullptr; // no enclosing cycle: nothing is relocated out of scope

  // The break-swallower count for a statement in `steps[i].block`, measured
  // against the target loop.
  auto swallowersAt = [&](size_t i) {
    unsigned count = 0;
    for (size_t j = i; j < target; ++j)
      if (steps[j].ownerIsLoop)
        ++count;
    return count;
  };

  // (2) walk back IN, from the target loop's body to the declaration.
  bool exitOnly = false;
  for (size_t i = target + 1; i-- > 0;) {
    unsigned swallowers = swallowersAt(i);
    // Entering this block from the construct it belongs to: an `if` branch
    // (or an inner, back-edge-free loop body) that always leaves the target
    // loop is itself relocated.
    //
    // Both tests here use the UNDER-approximation, because this one flips
    // the decision: claiming the declaration travels with the relocated code
    // ACCEPTS the program, so it may only be claimed when certain. Measured:
    // with the over-approximation, `switch (i) { case 1: printf(..); return
    // i; }` in a droppy loop body was wrongly skipped and kept miscompiling
    // (`case one` printed after `dtor 1`) -- a switch body's compound is not
    // sequential, so "some statement in it leaves" does not prove the switch
    // does.
    if (i != target && !exitOnly)
      exitOnly = mustLeaveLoop(steps[i].block, swallowers);
    if (exitOnly)
      break;
    // Position inside the block: a later sibling that always leaves makes
    // everything ahead of it exit-only too.
    bool seen = false;
    for (const clang::Stmt *child : steps[i].block->body()) {
      if (child == steps[i].node) {
        seen = true;
        continue;
      }
      if (seen && mustLeaveLoop(child, swallowers)) {
        exitOnly = true;
        break;
      }
    }
    if (exitOnly)
      break;
  }
  if (exitOnly)
    return nullptr; // the declaration travels with the relocated code

  // (3) scan the declaration's own scope, from the declaration onward.
  unsigned swallowers = swallowersAtDecl;
  llvm::SmallVector<const clang::Stmt *, 8> children(steps[0].block->body());
  size_t start = 0;
  while (start < children.size() &&
         children[start] != static_cast<const clang::Stmt *>(declStmt))
    ++start;
  for (size_t i = start; i < children.size(); ++i) {
    bool childExits = false;
    for (size_t j = i + 1; j < children.size() && !childExits; ++j)
      childExits = couldAlwaysLeaveLoop(children[j], swallowers);
    // The declaration ITSELF is never the offender: its constructor and its
    // `let` are relocated together with the drop they pair with, so their
    // relative order is preserved. (An OUTER droppy local sees that same
    // constructor as a relocated effect -- and catches it -- through its own
    // run of this check.)
    if (i == start)
      continue;
    if (const clang::Stmt *found =
            relocatedEffect(children[i], swallowers, childExits))
      return found;
  }
  return nullptr;
}

LogicalResult CImporter::checkDropLocalScope(const clang::VarDecl *var,
                                             Location loc) {
  clang::ASTContext &context = astContext();
  auto rejectScope = [&]() -> LogicalResult {
    return emitError(loc) << "unsupported: object of a class with a destructor "
                             "outside a function, loop, or branch body";
  };
  const clang::DeclStmt *declStmt = nullptr;
  for (const clang::DynTypedNode &parent : context.getParents(*var))
    if (const auto *candidate = parent.get<clang::DeclStmt>())
      declStmt = candidate;
  if (!declStmt)
    return rejectScope(); // a for-init condition variable, a catch handler, ...
  // FR-193 item 6: the outward walk RECORDS itself, because the loop-exit
  // fence below has to walk back IN -- from the loop whose exit paths get
  // relocated, down to this declaration -- to decide whether the declaration
  // travels with them.
  llvm::SmallVector<DropScopeStep, 4> steps;
  const clang::Stmt *node = declStmt;
  // Walk outward one enclosing block at a time. Every step must land on a
  // block whose Rust rendering is a real `{ ... }` at the same nesting.
  for (;;) {
    const clang::CompoundStmt *block = enclosingBlock(context, node);
    if (!block)
      return rejectScope(); // for-init, a switch case label, a bare statement
    const clang::Stmt *stmtParent = nullptr;
    const clang::FunctionDecl *funcParent = nullptr;
    for (const clang::DynTypedNode &parent : context.getParents(*block)) {
      if (const auto *asStmt = parent.get<clang::Stmt>())
        stmtParent = asStmt;
      if (const auto *asFunc = parent.get<clang::FunctionDecl>())
        funcParent = asFunc;
    }
    if (funcParent) {
      // The function body itself. A lambda's `operator()` is also a
      // FunctionDecl; its capture/lift machinery is out of this subset.
      const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(funcParent);
      if (method && method->getParent()->isLambda())
        return rejectScope();
      if (containsGotoOrLabel(funcParent->getBody()))
        return rejectScope();
      break;
    }
    if (const auto *forStmt =
            llvm::dyn_cast_or_null<clang::ForStmt>(stmtParent)) {
      if (forStmt->getBody() != block)
        return rejectScope();
      // FR-193 item 6 widened the screen from `containsCall` to the full
      // observable-effect predicate: `for (i = 0; i < n; ++i, tick++)` with a
      // destructor that reads the global `tick` printed `dtor 0 tick=1`
      // against the native's `dtor 0 tick=0` -- the same miscompile this gate
      // exists for, reached by a store instead of a call.
      if (observableEffect(forStmt->getInc()))
        return emitError(loc)
               << "unsupported: object of a class with a destructor in a loop "
                  "whose increment has side effects";
      steps.push_back({block, node, /*ownerIsLoop=*/true});
      node = forStmt;
      continue;
    }
    if (const auto *whileStmt =
            llvm::dyn_cast_or_null<clang::WhileStmt>(stmtParent)) {
      if (whileStmt->getBody() != block)
        return rejectScope();
      steps.push_back({block, node, /*ownerIsLoop=*/true});
      node = whileStmt;
      continue;
    }
    if (const auto *doStmt =
            llvm::dyn_cast_or_null<clang::DoStmt>(stmtParent)) {
      if (doStmt->getBody() != block)
        return rejectScope();
      // FR-193 item 6: a do-while CONDITION is the `for` increment's twin --
      // C++ destroys the body's locals BEFORE evaluating it, and the emitted
      // loop renders it at the bottom of the body, AHEAD of the drop.
      // Measured on `do { T a(i); printf("body"); ++i; } while (g(i) < n);`:
      // the native's `body 0 / dtor 0 / cond 1` came out as
      // `body 0 / cond 1 / dtor 0`. A plain `while` is NOT affected -- its
      // condition runs at the top of the iteration, after the previous drop,
      // and is byte-identical today.
      if (observableEffect(doStmt->getCond()))
        return emitError(loc)
               << "unsupported: object of a class with a destructor in a "
                  "do-while loop whose condition has side effects";
      steps.push_back({block, node, /*ownerIsLoop=*/true});
      node = doStmt;
      continue;
    }
    if (const auto *ifStmt =
            llvm::dyn_cast_or_null<clang::IfStmt>(stmtParent)) {
      if (ifStmt->getThen() != block && ifStmt->getElse() != block)
        return rejectScope();
      steps.push_back({block, node, /*ownerIsLoop=*/false});
      node = ifStmt;
      continue;
    }
    return rejectScope(); // bare nested block, switch case block, ...
  }
  // FR-193 item 6: the scope is modelled, but the LIFT can still move a side
  // effect across this object's drop. Rejection is a feature -- the
  // alternative here is a compile-clean crate that prints the destructor
  // trace in the wrong order.
  if (const clang::Stmt *effect = relocatedExitEffect(steps, declStmt)) {
    InFlightDiagnostic diag =
        emitError(loc) << "unsupported: object of a class with a destructor in "
                          "a loop whose exit path has side effects";
    diag.attachNote(translateLoc(effect->getBeginLoc()))
        << "this side effect runs before the destructor in C++, but it is on a "
           "path that only leaves the loop, so it is emitted after the loop -- "
           "past the drop";
    return failure();
  }
  return success();
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
  // W2.21: `int *q = p.get();` / `int *q = p.release();`. The pointer
  // PLANNER claims this local long before `emitStlMemberCall` runs and
  // reports its generic "pointer assigned a non-address value"; the real
  // blocker is that a raw pointer out of a Box has no representation at
  // all (a raw `T *` bound to a local is scalarized away entirely in this
  // model, measured), and release() additionally LEAKS the payload unless
  // the caller frees it.
  if (const clang::Expr *init = var->getInit())
    if (llvm::StringRef rawCall = matchStlBoxRawPointerCall(init);
        !rawCall.empty())
      return emitError(loc) << "unsupported: std::unique_ptr::" << rawCall
                            << "() hands out a raw pointer to the payload, "
                               "which has no place in this model";
  // W2.17: a destructor-carrying object is admitted only where its Rust
  // `Drop` runs at the same program point as the C++ destructor. The three
  // rejected positions here are all measured SILENT miscompiles: a
  // function-local `static` is module-level state a Rust `static` never
  // drops; an ARRAY is destroyed in reverse index order by C++ and forward
  // order by Rust (and `[X; N]` repeat is rustc E0277 once `Copy` is gone);
  // an unmodeled scope moves the drop point (see `checkDropLocalScope`).
  // W2.26: transitive -- a droppy-DERIVED local (its class merely inherits
  // the destructor) takes every one of these gates identically.
  if (userOrInheritedDestructor(astContext(), var->getType())) {
    if (!var->hasLocalStorage())
      return emitError(loc) << "unsupported: global or static object of a "
                               "class with a destructor";
    if (astContext().getAsArrayType(var->getType()))
      return emitError(loc)
             << "unsupported: array of a class with a destructor";
    if (failed(checkDropLocalScope(var, loc)))
      return failure();
  }
  // W2.23: an array of a copy-ctor class is the same boundary W2.17 drew
  // for droppy arrays, one wave later: once `Copy` leaves the derive
  // (emitrust.has_copy_ctor), the `[T; N]` repeat initializer is rustc
  // E0277 -- caught here as a located rejection instead. The droppy-array
  // wording above stays first for the copy+dtor class.
  if (astContext().getAsArrayType(var->getType())) {
    clang::QualType element = var->getType();
    while (const clang::ArrayType *arrayType =
               astContext().getAsArrayType(element.getCanonicalType()))
      element = arrayType->getElementType();
    if (admittedCopyConstructor(element->getAsCXXRecordDecl()))
      return emitError(loc)
             << "unsupported: array of a class with a copy constructor";
  }
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
    //
    // FR-155: NOT in a function that has a label. There,
    // `createVariablePlace` hoists the `emitrust.variable` op to the entry
    // block (so a goto over a declaration cannot leave a later use
    // undominated), and an init attribute rides on the OP -- so the hoist
    // carries the initializer out of the loop with the declaration and the
    // per-iteration reset is silently lost. Adding a dead `goto`/label to a
    // function then changes its answer: `for (...) { int s = 0; s += i;
    // total += s; }` accumulated 0,1,3 instead of resetting. Falling through
    // to the general path below emits the place without an init and a
    // `storeToPlace` at the CURRENT insertion point, i.e. inside the loop
    // body, which is correct. The clippy::needless_late_init win is
    // deliberately surrendered in labelled functions: correctness outranks
    // the cosmetic, and only the labelled case pays. Pinned by
    // test/EndToEnd/label-hoist-reinit.c (byte-diff) and
    // test/Import/C/label-hoist-reinit.c (IR shape).
    if (placeBackedScalars.contains(var) && !isUnsignedInt(*mlirType) &&
        !currentHasLabels)
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
        // W2.21: a `std::unique_ptr` local. Its initializer is NOT a
        // CXXConstructExpr — `auto p = std::make_unique<T>(..)` is
        // ExprWithCleanups -> CXXBindTemporaryExpr -> CallExpr under C++17
        // guaranteed elision — so it diverts before the construct
        // dispatch below, which would reject it as an unrecognized
        // initializer without ever naming unique_ptr.
        if (auto boxType = llvm::dyn_cast<emitrust::OpaqueType>(*mlirType);
            boxType && isStlBoxOpaque(boxType))
          return emitStlBoxLocalInit(place, boxType, init, loc);
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
    // W2.21: a `std::unique_ptr` local with no significant initializer is a
    // DEFAULT-CONSTRUCTED (null) unique_ptr. A Rust `Box<T>` has no null
    // state, and letting it fall through here would leave the place
    // rendering the emitter's defensive `Default::default()` arm — a live,
    // non-null payload where C++ had none.
    if (isStlOpaque && isStlBoxOpaque(*mlirType))
      return emitError(loc)
             << "unsupported: a Box<T> cannot be null, so a "
                "default-constructed std::unique_ptr has no image";
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
  if (!ctor)
    return emitError(loc) << "unsupported: copy/move construction";
  if (ctor->isCopyOrMoveConstructor()) {
    // W2.23: `Plain b = a;` -- the place-init spelling of the TRIVIAL
    // whole-struct copy the rvalue path has always unwrapped -- stores the
    // loaded source whole. Gated off droppy classes (a Rust load is a MOVE
    // once `Copy` is gone: one destructor run where C++ has two -- the
    // same reason as the rvalue path's refusal) and off the
    // derived-to-base SLICE (`A b = d;` copies a base SUBOBJECT, not a
    // whole object; pinned in inheritance-invalid.cpp).
    const clang::Expr *source =
        construct->getNumArgs() == 1 ? construct->getArg(0)->IgnoreParens()
                                     : nullptr;
    bool slices = false;
    if (const auto *sourceCast =
            llvm::dyn_cast_or_null<clang::ImplicitCastExpr>(source))
      slices =
          sourceCast->getCastKind() == clang::CK_DerivedToBase ||
          sourceCast->getCastKind() == clang::CK_UncheckedDerivedToBase;
    if (ctor->isTrivial() && source && !slices &&
        !userOrInheritedDestructor(astContext(), construct->getType())) {
      FailureOr<Value> whole = emitRValue(construct->getArg(0));
      if (failed(whole))
        return failure();
      return storeToPlace(loc, place, *whole);
    }
    // W2.23: the ADMITTED user copy constructor (`T b = a;`, 1 copy by the
    // standard in every mode -- byte-diffed in
    // test/EndToEnd/cpp-copy-ctor.cpp) is an ordinary imported method and
    // falls through to the constructor-call path below. Everything else --
    // a move construct, an IMPLICIT copy ctor made non-trivial by a
    // copy-ctor member or base (synthesizing a constructor the AST does
    // not contain is recorded and deferred), a droppy trivial copy --
    // keeps the located rejection.
    if (!(ctor->isCopyConstructor() && ctor->isUserProvided() &&
          functions.lookup(cxxMethodMangledName(ctor))))
      return emitError(loc) << "unsupported: copy/move construction";
  }
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
    Type input = targetType.getInput(index + 1);
    // FR-48 mirror (W2.23): a reference parameter on a CONSTRUCTOR takes
    // exactly the borrow argument a method's does -- same
    // `emitBorrowArgument`, same `emitrust.addr_of`. This is what lets the
    // admitted copy constructor's `const T&` (and any user ctor's
    // reference parameter) bind a bare lvalue; before W2.23 this loop
    // passed every argument by value, which is why a struct lvalue into a
    // ctor's `const T&` was the "call argument type mismatch" rejection.
    // The one collision possible here is with the RECEIVER -- the place
    // under construction already holds slot 0's &mut borrow, so the
    // self-copy `T b = b;` is exactly the aliasing shape the method path
    // rejects.
    if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(input)) {
      // `return t;` marks the copy's source XVALUE (`const T` xvalue NoOp
      // -- clang's implicit-move overload resolution, which then still
      // selects the copy ctor since the class declares no move ctor), so
      // FR-48's is-an-lvalue discriminator inside emitBorrowArgument
      // would miss it; the NoOp peel recovers the named object, which is
      // all the shared borrow reads.
      const clang::Expr *borrowExpr = argExpr;
      while (const auto *noOp =
                 llvm::dyn_cast<clang::ImplicitCastExpr>(borrowExpr)) {
        if (noOp->getCastKind() != clang::CK_NoOp)
          break;
        borrowExpr = noOp->getSubExpr();
      }
      if (const clang::VarDecl *argRoot = placeExprRoot(borrowExpr))
        if (symbols.lookup(argRoot) == place)
          return emitError(loc)
                 << "unsupported: aliasing mutable reference argument and "
                    "method receiver";
      const clang::VarDecl *unusedRoot = nullptr;
      FailureOr<Value> reference =
          emitBorrowArgument(loc, borrowExpr, input, unusedRoot);
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
    // W2.18: a BASE initializer on an IMPLICIT (or `= default`) derived
    // default constructor. This arm was a MEASURED SILENT MISCOMPILE
    // before the wave -- the `continue` below skipped it, so
    // `struct Seed { int s; Seed() : s(7) {} }; struct Holder : Seed { int
    // extra; }; Holder h;` left `h.base.s` at the Rust zero (native 9,
    // Rust 2) with no diagnostic, no verifier failure and a crate that
    // built clean. The base's own construction is the same place-based
    // call `emitCXXConstructInit` builds everywhere else, applied to
    // `place.base` rather than to a field of `place`.
    if (init->isBaseInitializer()) {
      Location baseLoc = translateLoc(init->getSourceLocation());
      const auto *baseConstruct =
          llvm::dyn_cast<clang::CXXConstructExpr>(init->getInit());
      if (!baseConstruct)
        return emitError(baseLoc)
               << "unsupported: base constructor initializer";
      const clang::CXXRecordDecl *baseRecord =
          init->getBaseClass()->getAsCXXRecordDecl();
      if (baseRecord && baseRecord->hasDefinition() && baseRecord->isEmpty()) {
        // An EMPTY base has no `base` field to construct into (see
        // `collectRecordFields`). A TRIVIAL construction of it has no
        // observable effect, so there is nothing to emit; a user-provided
        // constructor body would be silently DROPPED, so it rejects.
        const clang::CXXConstructorDecl *baseCtor =
            baseConstruct->getConstructor();
        if (baseCtor && baseCtor->isTrivial())
          continue;
        return emitError(baseLoc)
               << "unsupported: constructor of an empty base class";
      }
      FailureOr<Type> baseFieldType =
          mapType(init->getBaseClass()->getCanonicalTypeInternal(), baseLoc);
      if (failed(baseFieldType))
        return failure();
      Value basePlace =
          builder
              .create<emitrust::MemberOp>(
                  baseLoc, emitrust::LValueType::get(*baseFieldType), place,
                  builder.getStringAttr("base"))
              .getResult();
      if (failed(emitCXXConstructInit(basePlace, baseConstruct, baseLoc)))
        return failure();
      continue;
    }
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

LogicalResult
CImporter::emitImplicitCopyAssign(const clang::CXXOperatorCallExpr *call) {
  Location loc = translateLoc(call->getOperatorLoc());
  const auto *method =
      llvm::cast<clang::CXXMethodDecl>(call->getDirectCallee());
  const clang::RecordDecl *definition = method->getParent()->getDefinition();
  // Scalar fields only. A RECORD member would need the member's OWN
  // assignment semantics recursively -- which is exactly the FR-112
  // enclosing-operator= channel: a member with a user `operator=` is what
  // makes this implicit operator= non-trivial and materializes it as a
  // callee in the first place (cpp-contained-member-invalid.cpp pins it).
  // Arrays, pointers, unions and bit-fields keep the same located
  // rejection; the wording says what the construct IS, replacing the
  // pre-W2.23 "omitted from class" message the spike flagged as
  // misleading (an implicit member was never omitted).
  bool memberwise = definition && !definition->isUnion();
  if (memberwise)
    for (const clang::FieldDecl *field : definition->fields()) {
      clang::QualType fieldType = field->getType().getCanonicalType();
      if (!(fieldType->isArithmeticType() || fieldType->isEnumeralType()) ||
          field->isBitField() || field->getName().empty()) {
        memberwise = false;
        break;
      }
    }
  if (!memberwise)
    return emitError(loc) << "unsupported: implicit copy assignment of a "
                             "class with non-scalar members";
  // The right-hand side arrives wrapped in the `const T` NoOp cast its
  // `const T&` parameter binding added; the named place underneath is what
  // the memberwise reads borrow.
  FailureOr<Value> target = emitLValue(stripLValueNoOp(call->getArg(0)));
  if (failed(target))
    return failure();
  FailureOr<Value> source = emitLValue(stripLValueNoOp(call->getArg(1)));
  if (failed(source))
    return failure();
  // Memberwise, in declaration order (the order the implicit operator=
  // assigns; observable only through field side effects, of which scalar
  // fields have none -- but the emitted Rust reads naturally this way).
  // Self-assignment (`m = m;`) degenerates to `m.f = m.f`, which is
  // exactly what the native memberwise operator= does.
  for (const clang::FieldDecl *field : definition->fields()) {
    FailureOr<Type> fieldType = mapType(field->getType(), loc);
    if (failed(fieldType))
      return failure();
    Value sourceField =
        builder
            .create<emitrust::MemberOp>(
                loc, emitrust::LValueType::get(*fieldType), *source,
                builder.getStringAttr(field->getName()))
            .getResult();
    Value fieldValue = loadPlace(loc, sourceField);
    Value targetField =
        builder
            .create<emitrust::MemberOp>(
                loc, emitrust::LValueType::get(*fieldType), *target,
                builder.getStringAttr(field->getName()))
            .getResult();
    if (failed(storeToPlace(loc, targetField, fieldValue)))
      return failure();
  }
  return success();
}

LogicalResult CImporter::emitStlBoxLocalInit(Value place,
                                             emitrust::OpaqueType boxType,
                                             const clang::Expr *init,
                                             Location loc) {
  Type payload = stlBoxPayloadType(boxType);
  if (!payload)
    return emitError(loc) << "unsupported: std::unique_ptr payload type";
  // C++17 guaranteed elision means there is no CXXConstructExpr wrapper on
  // a make_unique initializer at all: the shape is ExprWithCleanups ->
  // CXXBindTemporaryExpr -> CallExpr (AST-confirmed, clang 21.1.8 /
  // libstdc++ 15).
  const clang::Expr *e = init->IgnoreParenImpCasts();
  while (true) {
    if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(e)) {
      e = cleanups->getSubExpr()->IgnoreParenImpCasts();
      continue;
    }
    if (const auto *bind = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(e)) {
      e = bind->getSubExpr()->IgnoreParenImpCasts();
      continue;
    }
    if (const auto *materialize =
            llvm::dyn_cast<clang::MaterializeTemporaryExpr>(e)) {
      e = materialize->getSubExpr()->IgnoreParenImpCasts();
      continue;
    }
    break;
  }
  if (const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(e)) {
    const clang::CXXConstructorDecl *ctor = construct->getConstructor();
    if (ctor && ctor->isCopyOrMoveConstructor())
      return emitError(loc)
             << "unsupported: a moved-from std::unique_ptr is null and "
                "testable, but Rust cannot read a moved-from binding";
    // The DEFAULT constructor (and the nullptr_t one) is the nullability
    // boundary; `std::unique_ptr<T> p(new T(..));` is a different shape —
    // an owning-pointer adoption whose `new` expression this subset does
    // not model at all — and says so.
    if (!ctor || ctor->isDefaultConstructor() ||
        construct->getNumArgs() == 0 ||
        llvm::all_of(construct->arguments(), [](const clang::Expr *arg) {
          return llvm::isa<clang::CXXDefaultArgExpr>(arg) ||
                 arg->getType()->isNullPtrType();
        }))
      return emitError(loc)
             << "unsupported: a Box<T> cannot be null, so a "
                "default-constructed std::unique_ptr has no image";
    return emitError(loc)
           << "unsupported: this std::unique_ptr initializer shape is not "
              "supported (only std::make_unique<T>(args) is recognized)";
  }
  const auto *call = llvm::dyn_cast<clang::CallExpr>(e);
  const clang::FunctionDecl *callee = call ? call->getDirectCallee() : nullptr;
  if (!callee || !callee->isInStdNamespace() ||
      !callee->getDeclName().isIdentifier() ||
      callee->getName() != "make_unique")
    return emitError(loc)
           << "unsupported: this std::unique_ptr initializer shape is not "
              "supported (only std::make_unique<T>(args) is recognized)";
  // A SCALAR payload is one `Box::new(v)`. `std::make_unique<int>()`
  // VALUE-initializes, so the zero-argument spelling boxes a zero.
  if (!llvm::isa<emitrust::StructType>(payload)) {
    Value value;
    if (call->getNumArgs() == 0) {
      if (auto intType = llvm::dyn_cast<IntegerType>(payload))
        value = createScalarIntConstant(loc, intType, 0);
      else
        return emitError(loc)
               << "unsupported: this std::unique_ptr initializer shape is "
                  "not supported (only std::make_unique<T>(args) is "
                  "recognized)";
    } else if (call->getNumArgs() == 1) {
      FailureOr<Value> argument = emitRValue(call->getArg(0));
      if (failed(argument))
        return failure();
      // The forwarding-reference parameter (`Args&&`) applies NO
      // conversion, so the argument arrives at its own type; an
      // implicit-conversion surface is out of subset rather than silently
      // widened here.
      if ((*argument).getType() != payload)
        return emitError(loc)
               << "unsupported: std::make_unique argument type does not "
                  "match the std::unique_ptr payload";
      value = *argument;
    } else {
      return emitError(loc)
             << "unsupported: this std::unique_ptr initializer shape is not "
                "supported (only std::make_unique<T>(args) is recognized)";
    }
    Value boxed = builder
                      .create<emitrust::CallOpaqueOp>(
                          loc, TypeRange{Type(boxType)},
                          builder.getStringAttr("Box::new"),
                          /*args=*/ArrayAttr(), ValueRange{value})
                      .getResult(0);
    return storeToPlace(loc, place, boxed);
  }
  // A STRUCT payload takes the W2.17 two-step: `Box::new(T::default())`
  // establishes the storage, then the C++ constructor runs as an ordinary
  // `&mut self` method on the Box place (Rust auto-deref). This is exactly
  // the shape a plain `T t(args);` local already takes
  // (`emitCXXConstructInit`), lifted one indirection.
  auto structType = llvm::cast<emitrust::StructType>(payload);
  const clang::RecordType *record =
      call->getType().getCanonicalType()->getAs<clang::RecordType>();
  const auto *spec =
      record ? llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
                   record->getDecl())
             : nullptr;
  if (!spec || spec->getTemplateArgs().size() < 1 ||
      spec->getTemplateArgs()[0].getKind() != clang::TemplateArgument::Type)
    return emitError(loc)
           << "unsupported: std::unique_ptr shape could not be determined";
  const clang::CXXRecordDecl *payloadRecord =
      spec->getTemplateArgs()[0].getAsType()->getAsCXXRecordDecl();
  if (!payloadRecord || !payloadRecord->hasDefinition())
    return emitError(loc) << "unsupported: std::unique_ptr payload type";
  Value defaulted =
      builder
          .create<emitrust::CallOpaqueOp>(
              loc, TypeRange{payload},
              builder.getStringAttr((structType.getName() + "::default").str()),
              /*args=*/ArrayAttr(), ValueRange{})
          .getResult(0);
  Value boxed = builder
                    .create<emitrust::CallOpaqueOp>(
                        loc, TypeRange{Type(boxType)},
                        builder.getStringAttr("Box::new"),
                        /*args=*/ArrayAttr(), ValueRange{defaulted})
                    .getResult(0);
  if (failed(storeToPlace(loc, place, boxed)))
    return failure();
  // Constructor selection. The instantiated `std::make_unique<T, Args...>`
  // body is not imported, so the constructor is resolved here by ARITY over
  // the user-declared, non-copy/move set; an overload set that cannot be
  // resolved that way rejects rather than guessing.
  llvm::SmallVector<const clang::CXXConstructorDecl *, 2> candidates;
  for (const clang::CXXConstructorDecl *ctor : payloadRecord->ctors()) {
    if (ctor->isImplicit() || ctor->isDeleted() ||
        ctor->isCopyOrMoveConstructor())
      continue;
    if (ctor->getNumParams() != call->getNumArgs())
      continue;
    candidates.push_back(ctor);
  }
  if (candidates.empty()) {
    // `std::make_unique<T>()` on a class with NO user-declared default
    // constructor VALUE-initializes, which zeroes every member — the same
    // thing `T::default()` above already produced. An in-class member
    // initializer (NSDMI) would make that false, so it rejects.
    if (call->getNumArgs() == 0 &&
        !payloadRecord->hasUserProvidedDefaultConstructor()) {
      for (const clang::FieldDecl *field : payloadRecord->fields())
        if (field->hasInClassInitializer())
          return emitError(loc)
                 << "unsupported: std::make_unique of a class with in-class "
                    "member initializers and no constructor";
      return success();
    }
    return emitError(loc)
           << "unsupported: no std::make_unique constructor of '"
           << payloadRecord->getName()
           << "' matches the argument count";
  }
  if (candidates.size() != 1)
    return emitError(loc)
           << "unsupported: std::make_unique constructor overload of '"
           << payloadRecord->getName() << "' is ambiguous by arity";
  std::string ctorName = cxxMethodMangledName(candidates.front());
  func::FuncOp target = functions.lookup(ctorName);
  if (!target)
    return emitError(loc) << "unsupported: call to an unimported constructor '"
                          << ctorName << "'";
  FunctionType targetType = target.getFunctionType();
  if (call->getNumArgs() + 1 != targetType.getNumInputs())
    return emitError(loc)
           << "unsupported: constructor argument count mismatch";
  SmallVector<Value> arguments;
  for (auto [index, argExpr] : llvm::enumerate(call->arguments())) {
    Type input = targetType.getInput(index + 1);
    if (llvm::isa<emitrust::MutRefType, emitrust::RefType>(input))
      return emitError(loc)
             << "unsupported: reference argument to a constructor called "
                "through std::make_unique";
    FailureOr<Value> value = emitRValue(argExpr);
    if (failed(value))
      return failure();
    if ((*value).getType() != input)
      return emitError(loc) << "unsupported: call argument type mismatch";
    arguments.push_back(*value);
  }
  builder.create<emitrust::MethodCallOp>(loc, TypeRange{}, place,
                                         builder.getStringAttr(ctorName),
                                         arguments);
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
    // W2.20: the map/set families join the same zero-argument
    // construction shape (`let vN: BTreeMap<K, V> = BTreeMap::new();`).
    llvm::StringRef callee = "Vec::new";
    if (opaque.getValue() == "String")
      callee = "String::new";
    else if (opaque.getValue().starts_with("BTreeMap<"))
      callee = "BTreeMap::new";
    else if (opaque.getValue().starts_with("BTreeSet<"))
      callee = "BTreeSet::new";
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

//===----------------------------------------------------------------------===//
// W2.24: exceptions as Result threading
//===----------------------------------------------------------------------===//

FailureOr<Type> CImporter::throwsPayloadMlir(Location loc) {
  if (throwsPayloadMappedType)
    return throwsPayloadMappedType;
  FailureOr<Type> mapped = mapType(throwsPayloadClangType, loc);
  if (failed(mapped))
    return failure();
  // The W2.14 scalar set: floats and SIGNLESS (signed-C) integers — the
  // payloads whose arith constants back the discriminant match's yields.
  auto intType = llvm::dyn_cast<IntegerType>(*mapped);
  bool scalar =
      llvm::isa<FloatType>(*mapped) || (intType && intType.isSignless());
  std::optional<std::string> spelling = rustSpellingForElementType(*mapped);
  if (!scalar || !spelling)
    return emitError(loc) << "unsupported: thrown exception payload must be "
                             "a supported scalar type";
  throwsPayloadMappedType = *mapped;
  // `Throws_i32` — `ThrowsI32` under the idiomatic rename: the same
  // non_camel_case_types deny constraint W2.14 measured.
  throwsEnumName = typeRustName("Throws_" + *spelling);
  return throwsPayloadMappedType;
}

FailureOr<emitrust::DataEnumType>
CImporter::getOrCreateThrowsEnum(Location loc) {
  FailureOr<Type> payload = throwsPayloadMlir(loc);
  if (failed(payload))
    return failure();
  auto enumType =
      emitrust::DataEnumType::get(builder.getContext(), throwsEnumName);
  if (throwsEnumsEmitted.insert(throwsEnumName).second) {
    // Ok0/Err0 is a pinned FREE choice (W2.14's precedent spells V0/V1);
    // both variants carry one payload field "v" of the SHARED payload
    // type — the wave-1 return-type gate is what makes Ok and Err agree,
    // and the single-payload-match unwrap depends on it.
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    moduleBuilder.create<emitrust::DataEnumDefOp>(
        loc, moduleBuilder.getStringAttr(throwsEnumName),
        moduleBuilder.getStrArrayAttr({"Ok0", "Err0"}),
        moduleBuilder.getArrayAttr({moduleBuilder.getStrArrayAttr({"v"}),
                                    moduleBuilder.getStrArrayAttr({"v"})}),
        moduleBuilder.getArrayAttr(
            {moduleBuilder.getTypeArrayAttr({*payload}),
             moduleBuilder.getTypeArrayAttr({*payload})}));
  }
  return enumType;
}

Value CImporter::createThrowsValue(Location loc, bool isErr, Value payload) {
  auto enumType =
      emitrust::DataEnumType::get(builder.getContext(), throwsEnumName);
  return builder
      .create<emitrust::EnumVariantOp>(
          loc, enumType,
          FlatSymbolRefAttr::get(builder.getContext(), throwsEnumName),
          builder.getStringAttr(isErr ? "Err0" : "Ok0"), ValueRange{payload})
      .getResult();
}

emitrust::MatchOp CImporter::createThrowsMatch(
    Location loc, Value scrutinee, Type resultType,
    llvm::function_ref<void(unsigned index, Value payload)> buildArm) {
  auto match = builder.create<emitrust::MatchOp>(
      loc, resultType ? TypeRange{resultType} : TypeRange{}, scrutinee,
      builder.getStrArrayAttr({"Ok0", "Err0"}), /*caseRegionsCount=*/2);
  OpBuilder::InsertionGuard guard(builder);
  for (unsigned index = 0; index < 2; ++index) {
    Block &block = match.getCaseRegions()[index].emplaceBlock();
    Value payload = block.addArgument(throwsPayloadMappedType, loc);
    builder.setInsertionPointToStart(&block);
    buildArm(index, payload);
  }
  return match;
}

bool CImporter::throwsDeliveryAvailable() const {
  return !tryContexts.empty() || currentFunctionThrows;
}

LogicalResult CImporter::deliverThrow(Location loc, Value payload) {
  if (!tryContexts.empty()) {
    const ThrowsTryContext &context = tryContexts.back();
    if (context.payloadPlace)
      builder.create<emitrust::AssignOp>(loc, context.payloadPlace, payload);
    builder.create<cf::BranchOp>(loc, context.catchBlock);
    return success();
  }
  // Propagation: construct Err0 and return it — the pyramid's seed.
  // `emitrust.return` is HasParent<FuncOp>, so this func.return in a cf
  // block (structurized later by lift-cf-to-scf) is the ONLY spellable
  // route; the closure gates guarantee no cursor writebacks exist.
  Value err = createThrowsValue(loc, /*isErr=*/true, payload);
  emitCursorWritebacks(loc);
  builder.create<func::ReturnOp>(loc, err);
  return success();
}

FailureOr<Value> CImporter::unwrapThrowsResult(Location loc, Value result) {
  if (!throwsDeliveryAvailable())
    return emitError(loc)
           << "unsupported: call to a potentially-throwing function outside "
              "a try block in a function that cannot propagate exceptions";
  // TWO result-mode matches over the Copy carrier, NOT one statement-mode
  // match assigning a payload slot: the single-slot spelling is denied by
  // unused_assignments (measured — TranslateToRust's analyzeControl never
  // proves a match writes, so the dead initializer survives; design.md
  // W2.24 constraint 2, resolution (a): zero emitter change).
  Value disc = createThrowsMatch(loc, result, builder.getI1Type(),
                                 [&](unsigned index, Value payload) {
                                   builder.create<emitrust::YieldOp>(
                                       loc, ValueRange{createBoolConstant(
                                                loc, index == 1)});
                                 })
                   .getResult();
  Value pay = createThrowsMatch(loc, result, throwsPayloadMappedType,
                                [&](unsigned index, Value payload) {
                                  builder.create<emitrust::YieldOp>(
                                      loc, ValueRange{payload});
                                })
                  .getResult();
  Block *errBlock = createBlock();
  Block *contBlock = createBlock();
  builder.create<cf::CondBranchOp>(loc, disc, errBlock, ValueRange(),
                                   contBlock, ValueRange());
  builder.setInsertionPointToEnd(errBlock);
  if (failed(deliverThrow(loc, pay)))
    return failure();
  builder.setInsertionPointToEnd(contBlock);
  return pay;
}

/// Whether `stmt` lexically contains a bare `throw;` rethrow (nested try
/// statements cannot occur below: emitTryStmt rejects nesting first).
static bool stmtContainsRethrow(const clang::Stmt *stmt) {
  if (!stmt)
    return false;
  if (const auto *throwExpr = llvm::dyn_cast<clang::CXXThrowExpr>(stmt))
    if (!throwExpr->getSubExpr())
      return true;
  for (const clang::Stmt *child : stmt->children())
    if (stmtContainsRethrow(child))
      return true;
  return false;
}

/// The first destructor-carrying local declared under `stmt`, or null.
/// W2.24 divergence gate: C++ destroys a try-block local at try exit
/// (BEFORE the handler runs); the image's variable places are
/// function-scoped after structurization, so an observable destructor
/// would fire at function exit instead — rejected, never diverged.
static const clang::VarDecl *
findDestructorLocalIn(clang::ASTContext &context, const clang::Stmt *stmt) {
  if (!stmt)
    return nullptr;
  if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt))
    for (const clang::Decl *decl : declStmt->decls())
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
        if (userOrInheritedDestructor(context, var->getType()))
          return var;
  for (const clang::Stmt *child : stmt->children())
    if (const clang::VarDecl *var = findDestructorLocalIn(context, child))
      return var;
  return nullptr;
}

LogicalResult CImporter::emitTryStmt(const clang::CXXTryStmt *tryStmt) {
  Location loc = translateLoc(tryStmt->getTryLoc());
  // Wave-1 gates, each a located rejection.
  if (!currentTryAllowed)
    return emitError(loc) << "unsupported: try/catch outside a "
                             "translation-unit-level function";
  if (!tryContexts.empty() || !catchPayloadPlaces.empty())
    return emitError(loc) << "unsupported: a try statement nested inside "
                             "another try or catch";
  if (tryStmt->getNumHandlers() != 1)
    return emitError(loc) << "unsupported: a try statement with more than "
                             "one catch handler";
  const clang::CXXCatchStmt *handler = tryStmt->getHandler(0);
  const clang::VarDecl *exceptionVar = handler->getExceptionDecl();
  if (exceptionVar) {
    clang::QualType caughtType = exceptionVar->getType();
    if (caughtType->isReferenceType())
      return emitError(translateLoc(exceptionVar->getLocation()))
             << "unsupported: catch by reference";
    if (!astContext().hasSameUnqualifiedType(
            astContext().getCanonicalType(caughtType),
            throwsPayloadClangType))
      return emitError(translateLoc(exceptionVar->getLocation()))
             << "unsupported: catch of a type other than the thrown "
                "payload type";
  }
  if (const clang::VarDecl *droppy =
          findDestructorLocalIn(astContext(), tryStmt))
    return emitError(translateLoc(droppy->getLocation()))
           << "unsupported: a class with a destructor declared inside a "
              "try statement";
  FailureOr<Type> payloadType = throwsPayloadMlir(loc);
  if (failed(payloadType))
    return failure();
  // The caught payload lives in an ordinary variable place, bound to the
  // catch parameter; a payload-less handler (catch(...) with no rethrow)
  // needs no place at all — delivery is control-only, and an unread
  // store must never survive to fight the deny lints.
  bool handlerRethrows = stmtContainsRethrow(handler->getHandlerBlock());
  Value payloadPlace;
  if (exceptionVar || handlerRethrows)
    payloadPlace = createVariablePlace(
        loc, *payloadType,
        exceptionVar && !exceptionVar->getName().empty()
            ? mangleMemberName(exceptionVar->getName())
            : std::string());
  Block *catchBlock = createBlock();
  Block *contBlock = createBlock();
  tryContexts.push_back({payloadPlace, catchBlock});
  LogicalResult bodyResult = emitStmt(tryStmt->getTryBlock());
  // Popped BEFORE the handler imports: a throw inside a catch handler
  // escapes this try (the planner's scan mirrors this exactly).
  tryContexts.pop_back();
  if (failed(bodyResult))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, contBlock);
  builder.setInsertionPointToEnd(catchBlock);
  if (exceptionVar)
    symbols[exceptionVar] = payloadPlace;
  catchPayloadPlaces.push_back(payloadPlace);
  LogicalResult handlerResult = emitStmt(handler->getHandlerBlock());
  catchPayloadPlaces.pop_back();
  if (failed(handlerResult))
    return failure();
  if (!isTerminated(builder.getInsertionBlock()))
    builder.create<cf::BranchOp>(loc, contBlock);
  builder.setInsertionPointToEnd(contBlock);
  return success();
}

LogicalResult CImporter::emitThrowStmt(const clang::CXXThrowExpr *expr) {
  Location loc = translateLoc(expr->getThrowLoc());
  const clang::Expr *sub = expr->getSubExpr();
  if (!sub) {
    // `throw;` — rethrow: reload the innermost handler's payload and
    // deliver it onward (to an outer try in a caller, or as Err0).
    if (catchPayloadPlaces.empty() || !catchPayloadPlaces.back())
      return emitError(loc) << "unsupported: rethrow outside a catch "
                               "handler";
    if (!throwsDeliveryAvailable())
      return emitError(loc)
             << "unsupported: throw outside a try block in a function that "
                "cannot propagate exceptions";
    Value payload = loadPlace(loc, catchPayloadPlaces.back());
    if (failed(deliverThrow(loc, payload)))
      return failure();
    builder.setInsertionPointToEnd(createBlock());
    return success();
  }
  // The context check comes FIRST (before the payload evaluates): the
  // uncaught-exception rejection must be deterministic, and emission never
  // drops a throw — this backstop is what keeps the planner's syntactic
  // closure safe for every context it skips (methods, lambdas, main).
  if (!throwsDeliveryAvailable())
    return emitError(loc)
           << "unsupported: throw outside a try block in a function that "
              "cannot propagate exceptions";
  FailureOr<Type> payloadType = throwsPayloadMlir(loc);
  if (failed(payloadType))
    return failure();
  FailureOr<Value> payload = emitRValue(sub);
  if (failed(payload))
    return failure();
  if ((*payload).getType() != *payloadType) // Defensive; planThrows pinned
                                            // one payload type per TU.
    return emitError(loc) << "unsupported: thrown exception payload must "
                             "be a supported scalar type";
  if (failed(deliverThrow(loc, *payload)))
    return failure();
  // Continue in a fresh block; if it stays unreachable it is erased later.
  builder.setInsertionPointToEnd(createBlock());
  return success();
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
  // FR-107: a pointer-ARRAY member's `[i64; N]` slot run carries no
  // runtime information either — no element is bound to a target object —
  // so an all-null brace list (the `{0}` a real declaration writes) keeps
  // the place's zero default and emits nothing, exactly like the scalar
  // data-pointer member above. An element naming an ADDRESS would be a
  // binding nothing records, so it is a located rejection HERE rather
  // than a silently dropped initializer. Without this branch the field
  // fell to the union-pun tail's `mapType`, which reported the
  // out-of-position pointer wording at a brace that is in no bad
  // position at all.
  if (pointerArrayMemberType(astContext(), field->getType())) {
    const auto *list = llvm::dyn_cast<clang::InitListExpr>(element);
    if (!list)
      return emitError(elementLoc)
             << "unsupported: pointer-array struct member initializer";
    if (const clang::InitListExpr *semantic = list->getSemanticForm())
      list = semantic;
    for (unsigned i = 0, n = list->getNumInits(); i != n; ++i) {
      const clang::Expr *init = list->getInit(i);
      if (llvm::isa<clang::ImplicitValueInitExpr>(init))
        continue;
      if (!init->isNullPointerConstant(
              astContext(), clang::Expr::NPC_ValueDependentIsNull))
        return emitError(translateLoc(init->getBeginLoc()))
               << "unsupported: pointer-array struct member initializer";
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
    const clang::FunctionDecl *callee = facts.initCall->getDirectCallee();
    bool nullable =
        callee && famNullableReturnFns.contains(callee->getCanonicalDecl());
    FailureOr<Value> value = emitCall(facts.initCall);
    if (failed(value))
      return failure();
    if (nullable) {
      // FR-99: a NULLABLE allocator hands back `Option<S>`. The result lands
      // in a temp place (an SSA value cannot cross the guard's block edge),
      // and the payload `unwrap` runs either right here (the unguarded bind)
      // or in the recognized guard's continuation block (the guarded bind),
      // so the temp is moved exactly once on every path.
      FailureOr<emitrust::OpaqueType> optionType =
          famOptionOfStruct(*structType, loc);
      if (failed(optionType))
        return failure();
      if (!*value || (*value).getType() != Type(*optionType))
        return emitError(loc)
               << "unsupported: flexible-array record initializer";
      Value temp = createVariablePlace(loc, Type(*optionType));
      builder.create<emitrust::AssignOp>(loc, temp, *value);
      famOptionTemps[var] = temp;
      if (famNullableGuardedLocals.contains(var))
        return success();
      return emitFamNullableUnwrap(var, loc);
    }
    if (!*value || (*value).getType() != *structType)
      return emitError(loc)
             << "unsupported: flexible-array record initializer";
    builder.create<emitrust::AssignOp>(loc, place, *value);
    return success();
  }
  return emitFamTailFill(place, pointee->getAsRecordDecl(), facts.countExpr,
                         loc);
}

LogicalResult CImporter::emitFamNullableUnwrap(const clang::VarDecl *var,
                                               Location loc) {
  auto temp = famOptionTemps.find(var);
  Value place = symbols.lookup(var);
  if (temp == famOptionTemps.end() || !place)
    // Defensive; the binding installs both before any unwrap can run.
    return emitError(loc) << "unsupported: flexible-array record initializer";
  auto lvalue = llvm::dyn_cast<emitrust::LValueType>(place.getType());
  if (!lvalue || !llvm::isa<emitrust::StructType>(lvalue.getValueType()))
    return emitError(loc) << "unsupported: flexible-array record type";
  Value payload = builder
                      .create<emitrust::MethodCallOp>(
                          loc, TypeRange{lvalue.getValueType()}, temp->second,
                          builder.getStringAttr("unwrap"), ValueRange{})
                      .getResult(0);
  builder.create<emitrust::AssignOp>(loc, place, payload);
  famOptionTemps.erase(var);
  return success();
}

const clang::VarDecl *
CImporter::famNullableBoundLocal(const clang::Expr *expr) {
  const clang::Expr *e = stripTrivia(expr);
  while (const clang::Expr *sub = peelPointerCast(astContext(), e))
    e = stripTrivia(sub);
  const clang::VarDecl *var = asLoadedLocalVarRef(e);
  if (!var)
    return nullptr;
  auto it = famAllocLocals.find(var);
  if (it == famAllocLocals.end() || !it->second.initCall)
    return nullptr;
  const clang::FunctionDecl *callee = it->second.initCall->getDirectCallee();
  return callee && famNullableReturnFns.contains(callee->getCanonicalDecl())
             ? var
             : nullptr;
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
      // FR-146: every pointer local UNITED into one allocation region
      // shares ONE backing. `char *q = p;` unites q with p (the region
      // analysis is union-find over the sources), and a per-VARIABLE
      // backing gave q a private zeroed array — writes through one
      // pointer were invisible through the other, a silent miscompile
      // that reached emission. The region's alloc site is its stable
      // identity here; a second pointer whose pointee maps to a
      // different element type would reinterpret the same storage and
      // rejects located instead.
      Value backing;
      auto sharedIt = allocRegionBackings.find(region->allocSite);
      if (sharedIt != allocRegionBackings.end()) {
        backing = sharedIt->second;
        if (llvm::cast<emitrust::LValueType>(backing.getType())
                .getValueType() != backingType)
          return emitError(loc)
                 << "unsupported: pointer '" << var->getName()
                 << "' reinterprets a heap allocation at a different "
                    "element type";
      } else {
        backing = createVariablePlace(loc, backingType);
        allocRegionBackings[region->allocSite] = backing;
      }
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
      // FR-147 (defect found while admitting allocation-backed slice
      // ARGUMENTS): a SECOND pointer united into this allocation region
      // (`char *q = p + 3;`, `const char *z = walk(p);`) declares here
      // too, and the cursor-0 store above is its ONLY binding — its
      // initializer was silently DROPPED, so `q` read the allocation
      // from offset 0 instead of 3. That is a wrong-answer miscompile a
      // byte diff sees and `cargo build` cannot. The allocating
      // declaration itself keeps the store-0-and-stop path byte for
      // byte (the malloc call IS the binding, and re-running it through
      // `storePointerAssign` would emit a redundant re-zero); every
      // other initializer is now stored exactly like the assignment
      // form `q = p + 3` already was.
      if (const clang::Expr *init = var->getInit())
        if (!asAllocCall(init))
          return storePointerAssign(loc, var, init);
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
    // FR-120: an UPCAST binding (`Base *p = &d;`) keeps the whole
    // DERIVED object as the region base; every struct-place use site
    // reconciles the viewed base type by re-deriving the `base` hop
    // chain (`reconcileUpcastPlace`), so a unique single public
    // non-virtual chain admits here alongside exact type equality.
    // W2.19b: the chain check runs with `allowPolymorphic` — this is the
    // single-base degenerate branch (`region->bases.front()`), i.e. the
    // single-object region fact holds by construction: the bound
    // object's dynamic type is statically known, non-virtual uses
    // statically bind (exact), and virtual calls devirtualize against it
    // or keep the fence in `emitCXXMemberCall`. Multi-object regions
    // never reach here (the `bases.size() >= 2` model above keeps its
    // strict element-type uniformity and JOIN rejection).
    if (!wildcard &&
        !astContext().hasSameUnqualifiedType(pointee, base->getType()) &&
        !uniquePublicSingleBaseChain(base->getType(), pointee,
                                     /*allowPolymorphic=*/true))
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

  // FR-61f-12: inside a lifted `emitrust.for` body the cf lowering below is
  // UNAVAILABLE -- `createBlock` appends to the FUNCTION region, so the
  // `cf.cond_br` would name successors outside the region it lives in and the
  // verifier faults on a null successor. Emit the structured form instead:
  // `emitrust.if` is `SingleBlockImplicitTerminator<"emitrust::YieldOp">` with
  // `results = (outs)`, so both arms are statement-mode regions and nothing
  // flows out as a value. Nothing further is needed, because
  // `collectRangeForPlaceScalars` has already routed every automatic-storage
  // integer scalar the body names to an `emitrust.variable` place: a read or
  // write inside an arm is a plain `emitrust.load`/`emitrust.assign` pair, not
  // a `memref.alloca` that mem2reg could not promote across the region op.
  //
  // The placement matters. This sits AFTER the C++17 init/condition-variable
  // hoist and AFTER the constant-condition arm elision, both of which are
  // correct in either mode; and `emitCondition` already yields an i1, which
  // feeds `$condition` with no cast.
  if (liftedForDepth > 0) {
    // FR-99's deferred payload unwrap lands in the cf CONTINUATION, which the
    // structured form does not have. `rangeForBodyVarIsPlaceBacked` refuses
    // the nullable-FAM POINTER local before the `if` is ever examined, so this
    // is argued unreachable -- but a deferred item that reaches emission
    // unresolved must fail LOUDLY rather than be silently dropped (the
    // `emitrust.extern_decl` / FR-52 marker contract).
    if (const clang::VarDecl *bound = famNullableGuards.lookup(stmt))
      if (famOptionTemps.contains(bound))
        return emitError(loc)
               << "unsupported: flexible-array null guard inside a lifted "
                  "range-for body";
    auto ifOp = builder.create<emitrust::IfOp>(loc, *condition);
    {
      OpBuilder::InsertionGuard guard(builder);
      Block *thenBody = builder.createBlock(&ifOp.getThenRegion());
      builder.setInsertionPointToEnd(thenBody);
      if (failed(emitStmt(stmt->getThen())))
        return failure();
      // Belt and braces: every terminator-producing statement (`break`,
      // `continue`, `goto`, `return`) is fenced out of a lifted body by
      // `blocksRangeForLift`, so the arm cannot already be terminated.
      if (!isTerminated(builder.getInsertionBlock()))
        builder.create<emitrust::YieldOp>(loc);
    }
    if (const clang::Stmt *elseStmt = stmt->getElse()) {
      OpBuilder::InsertionGuard guard(builder);
      Block *elseBody = builder.createBlock(&ifOp.getElseRegion());
      builder.setInsertionPointToEnd(elseBody);
      if (failed(emitStmt(elseStmt)))
        return failure();
      if (!isTerminated(builder.getInsertionBlock()))
        builder.create<emitrust::YieldOp>(loc);
    }
    return success();
  }

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
  // FR-99: the recognized guarded bind's DEFERRED payload unwrap lands in the
  // guard's continuation — `p = <temp>.unwrap()` — so the `None` arm never
  // touches the payload and the temp is moved exactly once.
  if (const clang::VarDecl *bound = famNullableGuards.lookup(stmt))
    if (famOptionTemps.contains(bound) &&
        failed(emitFamNullableUnwrap(bound, loc)))
      return failure();
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
  // W2.20: the container's OWN place, kept because `rangePlace` is
  // rebound to the ordered key snapshot below and the per-iteration value
  // lookup still has to index the map.
  Value mapPlace = rangePlace;
  auto rangeLValue =
      llvm::dyn_cast<emitrust::LValueType>(rangePlace.getType());
  if (!rangeLValue)
    return emitError(loc)
           << "unsupported: ranged-for range must be a named local "
              "container";
  // Element type: a Vec<T> opaque, a std::array-mapped !emitrust.array,
  // or (W2.20) a BTreeMap/BTreeSet opaque.
  auto vecType =
      llvm::dyn_cast<emitrust::OpaqueType>(rangeLValue.getValueType());
  auto arrayType =
      llvm::dyn_cast<emitrust::ArrayType>(rangeLValue.getValueType());
  emitrust::OpaqueType containerType = vecType;
  bool isMapRange = vecType && isStlMapOpaque(vecType);
  bool isSetRange = vecType && isStlSetOpaque(vecType);
  Type elementType;
  Type mapValueType;
  if (vecType && isStlOpaqueType(vecType) &&
      vecType.getValue().starts_with("Vec<")) {
    llvm::StringRef spelling = vecType.getValue();
    elementType = parseStlElementType(spelling.substr(4, spelling.size() - 5));
    if (!elementType)
      return emitError(loc) << "unsupported: ranged-for element type";
  } else if (isMapRange) {
    if (!stlMapKeyValueTypes(vecType, elementType, mapValueType))
      return emitError(loc) << "unsupported: ranged-for element type";
  } else if (isSetRange) {
    elementType = stlSetElementType(vecType);
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
  // W2.20: a ranged-for over a std::map yields `std::pair<const K, V>`.
  // Only the STRUCTURED-BINDING form is admitted: `for (auto &kv : m)`
  // would need a Vec of synthesized Pair structs (materially more work
  // than two scalar places), and the by-reference binding form aliases
  // the source object, which the per-binding copies here cannot model.
  const clang::DecompositionDecl *decomp = nullptr;
  if (isMapRange) {
    decomp = llvm::dyn_cast<clang::DecompositionDecl>(loopVar);
    if (!decomp)
      return emitError(loc)
             << "unsupported: a ranged-for over a std::map requires a "
                "structured binding (`for (auto [k, v] : m)`)";
    if (decomp->getType()->isReferenceType())
      return emitError(loc) << "unsupported: structured binding by reference";
    if (llvm::size(decomp->bindings()) != 2)
      return emitError(loc) << "unsupported: ranged-for loop variable shape";
    byReference = false;
  } else if (!byReference) {
    FailureOr<Type> mappedVar = mapType(loopVar->getType(), loc);
    if (failed(mappedVar))
      return failure();
    if (*mappedVar != elementType)
      return emitError(loc)
             << "unsupported: ranged-for loop variable type does not match "
                "the element type";
  }

  // W2.20: BTreeMap/BTreeSet have no positional index, so W2.10's index
  // desugar cannot walk them directly. The lowering takes an ORDERED KEY
  // SNAPSHOT first — `Vec::from_iter(BTreeMap::keys(&m).cloned())`, whose
  // every intermediate iterator type is NAMEABLE (the emitter's `let`
  // prologue demands an explicit annotation) — and then runs the EXISTING
  // index loop over that Vec unchanged. Ordered traversal is the whole
  // reason BTree is the right container, and the snapshot preserves it.
  // The snapshot also decouples the loop from the map's own borrow, which
  // the per-iteration value lookup needs.
  if (isMapRange || isSetRange) {
    std::optional<std::string> keySpelling =
        rustSpellingForElementType(elementType);
    if (!keySpelling)
      return emitError(loc) << "unsupported: ranged-for element type";
    std::string iterSpelling;
    llvm::StringRef iterCallee;
    if (isMapRange) {
      std::optional<std::string> valueSpelling =
          rustSpellingForElementType(mapValueType);
      if (!valueSpelling)
        return emitError(loc) << "unsupported: ranged-for element type";
      iterSpelling = "std::collections::btree_map::Keys<'_, " + *keySpelling +
                     ", " + *valueSpelling + ">";
      iterCallee = "std::collections::BTreeMap::keys";
    } else {
      iterSpelling =
          "std::collections::btree_set::Iter<'_, " + *keySpelling + ">";
      iterCallee = "std::collections::BTreeSet::iter";
    }
    MLIRContext *context = builder.getContext();
    Value containerRef =
        builder
            .create<emitrust::AddrOfOp>(loc,
                                        emitrust::RefType::get(containerType),
                                        rangePlace, /*is_mut=*/false)
            .getResult();
    Value iter = builder
                     .create<emitrust::CallOpaqueOp>(
                         loc,
                         TypeRange{emitrust::OpaqueType::get(context,
                                                             iterSpelling)},
                         builder.getStringAttr(iterCallee),
                         /*args=*/ArrayAttr(), ValueRange{containerRef})
                     .getResult(0);
    Value cloned =
        builder
            .create<emitrust::CallOpaqueOp>(
                loc,
                TypeRange{emitrust::OpaqueType::get(
                    context, "std::iter::Cloned<" + iterSpelling + ">")},
                builder.getStringAttr("std::iter::Iterator::cloned"),
                /*args=*/ArrayAttr(), ValueRange{iter})
            .getResult(0);
    auto snapshotType =
        emitrust::OpaqueType::get(context, "Vec<" + *keySpelling + ">");
    Value snapshot = builder
                         .create<emitrust::CallOpaqueOp>(
                             loc, TypeRange{snapshotType},
                             builder.getStringAttr("Vec::from_iter"),
                             /*args=*/ArrayAttr(), ValueRange{cloned})
                         .getResult(0);
    Value snapshotPlace = createVariablePlace(loc, snapshotType);
    if (failed(storeToPlace(loc, snapshotPlace, snapshot)))
      return failure();
    rangePlace = snapshotPlace;
    vecType = snapshotType;
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
  if (isMapRange) {
    // W2.20: the two structured-binding places. The key binding is a
    // fresh per-iteration COPY of the snapshot element (writing it must
    // not touch the container, and C++'s `auto [k, v]` binds to a copy of
    // the pair too); the value binding is looked up in the map by that
    // key — `*std::ops::Index::index(&m, &snapshot[i])` — and copied the
    // same way. Registering each BindingDecl in `symbols` is exactly what
    // `emitDecompositionDecl` does for the non-loop form.
    auto bindings = decomp->bindings();
    const clang::BindingDecl *keyBinding = bindings[0];
    const clang::BindingDecl *valueBinding = bindings[1];
    Value keyValue = loadPlace(loc, elementPlace);
    Value keyPlace = createVariablePlace(
        loc, elementType,
        keyBinding->getName().empty()
            ? std::string()
            : mangleMemberName(keyBinding->getName()));
    if (failed(storeToPlace(loc, keyPlace, keyValue)))
      return failure();
    Value mapRef =
        builder
            .create<emitrust::AddrOfOp>(loc,
                                        emitrust::RefType::get(containerType),
                                        mapPlace, /*is_mut=*/false)
            .getResult();
    Value keyRef =
        builder
            .create<emitrust::AddrOfOp>(loc,
                                        emitrust::RefType::get(elementType),
                                        elementPlace, /*is_mut=*/false)
            .getResult();
    Value slot = builder
                     .create<emitrust::CallOpaqueOp>(
                         loc, TypeRange{emitrust::RefType::get(mapValueType)},
                         builder.getStringAttr("std::ops::Index::index"),
                         /*args=*/ArrayAttr(), ValueRange{mapRef, keyRef})
                     .getResult(0);
    Value slotPlace =
        builder
            .create<emitrust::DerefOp>(
                loc, emitrust::LValueType::get(mapValueType), slot)
            .getResult();
    Value valueValue = loadPlace(loc, slotPlace);
    Value valuePlace = createVariablePlace(
        loc, mapValueType,
        valueBinding->getName().empty()
            ? std::string()
            : mangleMemberName(valueBinding->getName()));
    if (failed(storeToPlace(loc, valuePlace, valueValue)))
      return failure();
    symbols[keyBinding] = keyPlace;
    symbols[valueBinding] = valuePlace;
  } else if (byReference) {
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

/// FR-61f-c: whether the subtree DECLARES `var` (a `DeclStmt` naming it).
/// A pointer declared inside the loop body has its initializing store INSIDE
/// the `emitrust.for` region, so its cursor cannot be hoisted to a value that
/// dominates the region -- there is nothing to load yet.
bool stmtDeclaresVar(const clang::Stmt *stmt, const clang::VarDecl *var) {
  if (!stmt)
    return false;
  if (const auto *decls = llvm::dyn_cast<clang::DeclStmt>(stmt))
    for (const clang::Decl *decl : decls->decls())
      if (llvm::dyn_cast_or_null<clang::VarDecl>(decl) == var)
        return true;
  for (const clang::Stmt *child : stmt->children())
    if (stmtDeclaresVar(child, var))
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

//===--------------------------------------------------------------------===//
// FR-61f-5 SPIKE: assignment-form init (`for (i = LO; ...)`), guarded by an
// AST "induction is dead after the loop" proof.
//===--------------------------------------------------------------------===//

/// Whether `stmt` READS `var`. Every `DeclRefExpr` to `var` counts as a read
/// EXCEPT the LHS of a simple assignment `var = E` (a pure definition).
/// `var++`, `var += K` and `&var` all count as reads, deliberately: the
/// obligation is one-directional (a wrong "dead" answer silently drops a
/// value), so anything not provably a pure definition is a read.
bool readsVar(const clang::Stmt *stmt, const clang::VarDecl *var) {
  if (!stmt)
    return false;
  if (const auto *bo = llvm::dyn_cast<clang::BinaryOperator>(stmt))
    if (bo->getOpcode() == clang::BO_Assign && isRefTo(bo->getLHS(), var))
      return readsVar(bo->getRHS(), var);
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
    return ref->getDecl() == var;
  for (const clang::Stmt *child : stmt->children())
    if (readsVar(child, var))
      return true;
  return false;
}

/// `readsVar` over `stmt` with the whole `skip` subtree excluded.
bool readsVarOutside(const clang::Stmt *stmt, const clang::Stmt *skip,
                     const clang::VarDecl *var) {
  if (!stmt || stmt == skip)
    return false;
  if (const auto *bo = llvm::dyn_cast<clang::BinaryOperator>(stmt))
    if (bo->getOpcode() == clang::BO_Assign && isRefTo(bo->getLHS(), var))
      return readsVarOutside(bo->getRHS(), skip, var);
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt))
    return ref->getDecl() == var;
  for (const clang::Stmt *child : stmt->children())
    if (readsVarOutside(child, skip, var))
      return true;
  return false;
}

/// Whether `needle` occurs anywhere in `stmt`.
bool containsStmt(const clang::Stmt *stmt, const clang::Stmt *needle) {
  if (!stmt)
    return false;
  if (stmt == needle)
    return true;
  for (const clang::Stmt *child : stmt->children())
    if (containsStmt(child, needle))
      return true;
  return false;
}

/// Any construct that can move control out of the straight-line sibling
/// sequence. The sibling walk stops (conservatively, without concluding
/// "dead") when it meets one, because a statement it skips may be the very
/// redefinition the walk would otherwise have counted as a kill.
bool transfersControl(const clang::Stmt *stmt) {
  if (!stmt)
    return false;
  if (llvm::isa<clang::BreakStmt, clang::ContinueStmt, clang::ReturnStmt,
                clang::GotoStmt, clang::IndirectGotoStmt>(stmt))
    return true;
  for (const clang::Stmt *child : stmt->children())
    if (transfersControl(child))
      return true;
  return false;
}

/// A `goto`, a label, or `&&label` anywhere in the function invalidates the
/// syntactic "what runs after" argument entirely: refuse outright.
bool containsJumpMachinery(const clang::Stmt *stmt) {
  if (!stmt)
    return false;
  if (llvm::isa<clang::GotoStmt, clang::IndirectGotoStmt, clang::LabelStmt,
                clang::AddrLabelExpr>(stmt))
    return true;
  for (const clang::Stmt *child : stmt->children())
    if (containsJumpMachinery(child))
      return true;
  return false;
}

/// Whether `stmt` UNCONDITIONALLY rewrites the WHOLE of `var` on entry,
/// before anything in `stmt` can read it. Exactly two shapes qualify: a bare
/// `var = E;`, and a `for (var = E; ...)` whose init runs exactly once,
/// first, on entry. `E` itself must not read `var`.
bool killsVarUpFront(const clang::Stmt *stmt, const clang::VarDecl *var) {
  if (!stmt)
    return false;
  const clang::Stmt *head = stmt;
  if (const auto *forStmt = llvm::dyn_cast<clang::ForStmt>(stmt))
    head = forStmt->getInit();
  const auto *headExpr = llvm::dyn_cast_or_null<clang::Expr>(head);
  if (!headExpr)
    return false;
  const auto *bo =
      llvm::dyn_cast<clang::BinaryOperator>(headExpr->IgnoreParenImpCasts());
  return bo && bo->getOpcode() == clang::BO_Assign &&
         isRefTo(bo->getLHS(), var) && !readsVar(bo->getRHS(), var);
}

/// FR-61f-f. Whether `stmt` can read the value `var` holds on ENTRY to
/// `stmt`. Strictly weaker than `readsVar`: a statement may name `var` many
/// times and still never observe the INCOMING value, because a definition
/// inside `stmt` dominates every one of those reads.
///
/// This is deliberately NOT a claim that `stmt` kills `var`. A nested
/// definition need not execute at all (a `for` may run its body zero times),
/// so a `false` answer here only ever downgrades a `Read` to a `Pass`, never
/// promotes anything to a `Kill`. The set of statements that can END
/// `walkAfter`'s scan with "dead" is therefore exactly what it was before,
/// and the `killsUnreliable` reasoning below is untouched.
///
/// `goto` is not reasoned about here at all: `inductionDeadAfter` refuses
/// outright on any function containing jump machinery, so the only control
/// transfers that can reach this code are `break`/`continue`/`return`, which
/// can SKIP statements but never introduce a read.
///
/// Anything not modelled answers `true` (observes), i.e. refuses to lift.
bool observesIncomingValue(const clang::Stmt *stmt, const clang::VarDecl *var) {
  if (!readsVar(stmt, var))
    return false; // never names `var` as a read at all
  if (killsVarUpFront(stmt, var))
    return false; // the definition dominates the whole statement

  // Sequenced and unconditional: scan in order. The first child that kills
  // `var` outright ends the question -- nothing after it can see the incoming
  // value -- and a child that merely does not observe it lets the scan
  // continue to its siblings.
  if (const auto *comp = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
    for (const clang::Stmt *child : comp->body()) {
      if (killsVarUpFront(child, var))
        return false;
      if (observesIncomingValue(child, var))
        return true;
    }
    return false;
  }

  // A `for` whose own init does NOT define `var`. Its init runs first and
  // exactly once; cond, body and inc then run repeatedly. If none of
  // init/cond/inc names `var`, every read of `var` in the statement is in the
  // BODY, and the body is asked the same question. Repetition is harmless: a
  // body that does not observe the value it is entered with cannot observe
  // the loop's incoming value on the first pass, and on later passes it sees
  // only what earlier passes left.
  //
  // This is the FR-61f-f case. `for (i = 0; i < 8; i++) for (j = 0; ..) ..`
  // names `j` in three places, but all three are dominated by the INNER
  // `j = 0`, so the outer statement does not observe the `j` that an earlier
  // loop pair left behind -- even though it is not a kill of `j` either,
  // because the outer loop may run zero times.
  //
  // `while` and `do` are absent on purpose. A `while` has no init slot, and
  // its condition is evaluated BEFORE its body, so a definition in the body
  // cannot dominate it; a `do` runs its body first but the same body-then-
  // condition reasoning has not been shown correct here. Both fall through to
  // `true` below and keep refusing.
  if (const auto *forStmt = llvm::dyn_cast<clang::ForStmt>(stmt)) {
    if (readsVar(forStmt->getInit(), var) ||
        readsVar(forStmt->getCond(), var) || readsVar(forStmt->getInc(), var))
      return true;
    return observesIncomingValue(forStmt->getBody(), var);
  }

  return true;
}

/// The effect of executing `stmt` on `var`'s liveness at the point just
/// before `stmt`. `Kill` means: on EVERY path through `stmt`, `var` is
/// redefined before it is read -- only the two shapes whose redefinition
/// unconditionally dominates the statement qualify (see `killsVarUpFront`).
/// `Pass` means `stmt` cannot observe the value `var` arrives with; `Read`
/// means it may.
enum class VarEffect { Pass, Kill, Read };

VarEffect effectOn(const clang::Stmt *stmt, const clang::VarDecl *var) {
  if (!stmt)
    return VarEffect::Pass;
  if (killsVarUpFront(stmt, var))
    return VarEffect::Kill;
  return observesIncomingValue(stmt, var) ? VarEffect::Read : VarEffect::Pass;
}

/// Outcome of the walk that locates `target` in the function body and then
/// inspects everything that can run after it.
///   NotFound -- `target` is not in this subtree.
///   Open     -- found, and nothing seen so far reads `var`; the enclosing
///               scope must keep looking.
///   Dead     -- found, and a redefinition of `var` dominates every later
///               read: stop, the lift is safe.
///   Refuse   -- found, and something that may run afterwards reads `var`
///               (or the construct is not modelled).
enum class DeadWalk { NotFound, Open, Dead, Refuse };

DeadWalk walkAfter(const clang::Stmt *stmt, const clang::ForStmt *target,
                   const clang::VarDecl *var) {
  if (!stmt)
    return DeadWalk::NotFound;
  if (stmt == target)
    return DeadWalk::Open;

  if (const auto *comp = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
    for (auto it = comp->body_begin(), end = comp->body_end(); it != end;
         ++it) {
      DeadWalk inner = walkAfter(*it, target, var);
      if (inner == DeadWalk::NotFound)
        continue;
      if (inner != DeadWalk::Open)
        return inner;
      // A `break`/`continue`/`return`/`goto` in a later sibling can skip the
      // statements between it and the end of this compound, so a
      // redefinition placed after it no longer dominates the reads that
      // follow. Once one is seen, keep scanning for READS (they still refuse)
      // but stop trusting KILLS. Abandoning the scan instead was the spike's
      // measured miscompile: `for (i=0;i<n;i++){s+=i;} if (c) return s;
      // return s*100+i;` lifted and printed 300 where clang printed 303.
      bool killsUnreliable = false;
      for (auto rest = std::next(it); rest != end; ++rest) {
        VarEffect effect = effectOn(*rest, var);
        if (effect == VarEffect::Read)
          return DeadWalk::Refuse;
        if (effect == VarEffect::Kill && !killsUnreliable)
          return DeadWalk::Dead;
        if (transfersControl(*rest))
          killsUnreliable = true;
      }
      return DeadWalk::Open;
    }
    return DeadWalk::NotFound;
  }

  // Transparent: nothing inside the other arm / the condition runs after the
  // target, so the enclosing compound's sibling walk is the whole story.
  if (const auto *ifStmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
    for (const clang::Stmt *arm : {ifStmt->getThen(), ifStmt->getElse()}) {
      DeadWalk inner = walkAfter(arm, target, var);
      if (inner != DeadWalk::NotFound)
        return inner;
    }
    return containsStmt(ifStmt, target) ? DeadWalk::Refuse
                                        : DeadWalk::NotFound;
  }
  if (const auto *sw = llvm::dyn_cast<clang::SwitchStmt>(stmt)) {
    DeadWalk inner = walkAfter(sw->getBody(), target, var);
    if (inner != DeadWalk::NotFound)
      return inner;
    return containsStmt(sw, target) ? DeadWalk::Refuse : DeadWalk::NotFound;
  }
  if (const auto *caseStmt = llvm::dyn_cast<clang::SwitchCase>(stmt)) {
    DeadWalk inner = walkAfter(caseStmt->getSubStmt(), target, var);
    if (inner != DeadWalk::NotFound)
      return inner;
    return DeadWalk::NotFound;
  }

  // An ENCLOSING loop re-runs everything it contains, so every read of `var`
  // anywhere in it (outside the target) happens after the target on some
  // path. No kill reasoning applies here -- a kill that precedes the target
  // in program order does not kill the value the target leaves behind.
  if (llvm::isa<clang::ForStmt, clang::WhileStmt, clang::DoStmt,
                clang::CXXForRangeStmt>(stmt)) {
    if (!containsStmt(stmt, target))
      return DeadWalk::NotFound;
    if (readsVarOutside(stmt, target, var))
      return DeadWalk::Refuse;
    return DeadWalk::Open;
  }

  // Anything else that contains the target is not modelled: refuse.
  return containsStmt(stmt, target) ? DeadWalk::Refuse : DeadWalk::NotFound;
}

/// Clause 6 for the assignment form: `var` is provably never read again once
/// `target` exits. Refuses by default on everything it cannot prove.
bool inductionDeadAfter(const clang::Stmt *funcBody,
                        const clang::ForStmt *target,
                        const clang::VarDecl *var) {
  if (!funcBody || containsJumpMachinery(funcBody))
    return false;
  DeadWalk result = walkAfter(funcBody, target, var);
  return result == DeadWalk::Dead || result == DeadWalk::Open;
}

} // namespace

/// Whether emitting `stmt` would create cf basic blocks in the function
/// region (nested control flow, short-circuit `&&`/`||`, `?:`, statement
/// expressions) or install a loop exit/jump edge (`break`/`continue`/`goto`/
/// `return`/labels/`case`). An `emitrust.for` body is a single-block region,
/// so any of these in the body makes the loop non-liftable in this slice.
///
/// THIS IS A BLOCKLIST, AND BLOCKLISTS LEAK. Two entries have now been found
/// the hard way, both by crashing the MLIR verifier rather than by a
/// diagnostic: `va_arg` (FR-61f-9) and a call to a can-throw-closure function
/// (here). So the invariant is written down. The complete set of emitters in
/// lib/ImportC that call `createBlock`/`getLabelBlock` or create a
/// `cf::BranchOp`/`cf::CondBranchOp`/`cf::SwitchOp` is:
///
///   emitIfStmt, emitWhileStmt, emitDoStmt, emitForStmt, emitSwitchStmt,
///   emitDispatchSwitch, emitMultiBaseDispatch, emitReturnStmt, emitStmt
///   (break/continue/goto/label), emitShortCircuit, emitConditionalOperator,
///   emitVaArg, emitThrowStmt, emitTryStmt, unwrapThrowsResult,
///   storePointerAssign, emitGlobalCursorWrite, getLabelBlock.
///
/// Every one is either fenced below or unreachable inside a lifted body, and
/// the ones that are NOT fenced are safe for a stated reason:
///   - emitIfStmt: as of FR-61f-12 it has a second, STRUCTURED arm (gated on
///     `liftedForDepth`) that emits an `emitrust.if` region pair and creates
///     no cf blocks at all. `clang::IfStmt` is therefore off the list, and the
///     generic child recursion below still fences anything hazardous nested
///     inside the arms -- a `break` in an `if` blocks exactly as before.
///   - emitForStmt routes to `emitRangeFor` (a region op) when the nested loop
///     itself lifts, and is fenced by the dyn_cast above when it does not.
///   - storePointerAssign, emitGlobalCursorWrite, emitMultiBaseDispatch and
///     emitDispatchSwitch all need a POINTER or REFERENCE variable in the
///     body, which `rangeForBodyVarIsPlaceBacked` refuses outright.
///
/// Anything added to lib/ImportC that creates a block belongs on that list or
/// in this fence.
///
/// THE ONE EXCEPTION is a nested `for` that ITSELF lifts. A lifted loop emits
/// a REGION OP, not cf blocks, and `emitrust.for` regions nest by
/// construction -- `emitRangeFor` already saves and restores its
/// `inductionValues` entry precisely so a nested body may read the enclosing
/// induction. A nested loop that does NOT lift still blocks, because it would
/// emit the cf `while` lowering inside the single-block region.
///
/// This is what keeps the aggregate widening from being a net loss. Without
/// it, lifting an INNER loop drags the OUTER loop's induction into
/// `placeBackedScalars`; the outer loop then has no SSA loop-carried value
/// for `lift-cf-to-scf` to recognize and degrades from a clean `while` to
/// `loop { let c = ..; if c { .. } if !c { break; } }`. Measured: 12 such
/// degradations across the EndToEnd corpus. Lifting the outer loop too turns
/// each of them into a `for` head instead.
///
/// Recursion terminates: the nested `matchRangeFor` runs on a STRICTLY more
/// deeply nested statement.
bool CImporter::blocksRangeForLift(const clang::Stmt *stmt) {
  if (!stmt)
    return false;
  if (const auto *nested = llvm::dyn_cast<clang::ForStmt>(stmt))
    return !matchRangeFor(nested).has_value();
  // `clang::VAArgExpr`: `va_arg` lowers to control flow of its own (the
  // register-save-area walk), so it cannot live in the single-block region.
  // It is listed here rather than left to the body whitelist because the
  // whitelist is a statement about VARIABLES and this is a statement about an
  // EXPRESSION -- `va_arg(ap, int)` names `ap`, but the hazard is the walk,
  // not the `va_list`. (The whitelist fences the `va_list` too; both fire,
  // and both are meant to.)
  //
  // `clang::CXXThrowExpr` / `clang::CXXTryStmt`: `emitThrowStmt` and
  // `emitTryStmt` build the W2.24 Result-threading cf pattern, which is cf
  // blocks in the function region like any other.
  if (llvm::isa<clang::WhileStmt,
                clang::DoStmt, clang::SwitchStmt, clang::CXXForRangeStmt,
                clang::BreakStmt, clang::ContinueStmt, clang::GotoStmt,
                clang::IndirectGotoStmt, clang::ReturnStmt, clang::LabelStmt,
                clang::CaseStmt, clang::DefaultStmt,
                clang::ConditionalOperator, clang::BinaryConditionalOperator,
                clang::VAArgExpr, clang::StmtExpr,
                clang::CXXThrowExpr, clang::CXXTryStmt>(stmt))
    return true;
  // A CALL to a can-throw-closure function is a block-creating construct too,
  // and NOTHING ABOUT ITS SPELLING SAYS SO -- which is exactly why this
  // blocklist was missing it. `unwrapThrowsResult` emits two `createBlock()`s
  // and a `cf::CondBranchOp` for the early-return pattern, and `createBlock`
  // appends to the FUNCTION region; with the insertion point inside the
  // single-block `emitrust.for` region the resulting `cf.cond_br` names
  // successors that are not in it, and the MLIR verifier faults on the null
  // successor. `throwsClosure` is a module-level planning result, stable
  // across the pre-pass and emission, so reading it here preserves
  // `matchRangeFor`'s purity the same way `addressTaken` does; the predicate
  // mirrors the call site in `emitCallExpr`.
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt))
    if (throwsPlanActive)
      if (const clang::FunctionDecl *callee = call->getDirectCallee())
        if (throwsClosure.contains(callee->getCanonicalDecl()))
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

// True when an automatic-storage variable a range-eligible `for` body
// touches materializes as an `emitrust.variable` PLACE rather than a
// `memref.alloca` cell. A cell inside the single-block `emitrust.for`
// region is un-promotable by mem2reg and `convert-to-emitrust` then
// rejects it (spike 61f-0), so this predicate IS the body whitelist.
//
// It mirrors, in clang-type terms, the place/cell decision the three cell
// producers actually make -- `emitLocalVar` (isAggregate || isPlaceOnly ||
// unsigned || address-taken || placeBackedScalars),
// `bindOrdinaryParam` (StructType/EnumType/FnPtrType/OpaqueType || ...)
// and `bindLiftedCaptureValue`. It is stated on the AST type ON PURPOSE:
// `mapType` imports records as a side effect, and calling it from a
// speculative matcher (which runs twice per ForStmt, and once more from
// `collectRangeForPlaceScalars` before any body is emitted) would shift
// `struct_def` emission order in the module.
bool CImporter::rangeForBodyVarIsPlaceBacked(const clang::VarDecl *var) {
  clang::QualType type = var->getType().getCanonicalType();

  // Integer scalars (chars and `_Bool` included): a local or a plain
  // signed PARAMETER gets its place from `placeBackedScalars`, which
  // `collectRangeForPlaceScalars` fills for exactly this set; unsigned and
  // address-taken scalars were already places.
  if (type->isIntegerType())
    return true;
  // Enums map to `!emitrust.enum` and function pointers to
  // `!emitrust.fn_ptr`; a memref of a dialect type is illegal, so both
  // were always places on every arm.
  if (type->isEnumeralType() || type->isFunctionPointerType())
    return true;
  // A `va_list` is `struct __va_list_tag[1]` on the SysV ABI, so it would
  // sail through the array arm below -- and a body that walks it emits the
  // register-save-area control flow, which broke the region verifier
  // outright (a SEGFAULT in `verifyNSuccessors`, not a diagnostic). This was
  // a real regression from the aggregate widening, latent only because the
  // one corpus instance also touched a global and so was rejected earlier in
  // the chain; the c-testsuite `<stdarg.h>` cases reach it directly.
  if (!astContext().getBuiltinVaListType().isNull() &&
      type == astContext().getBuiltinVaListType().getCanonicalType())
    return false;
  // A CONSTANT array of ANY element type maps to `!emitrust.array`
  // (`mapType` recurses through every dimension), which `emitLocalVar`
  // routes to `createVariablePlace` via `isAggregate` -- so `int[3][4]`,
  // `struct P[4]` and `double[5]` are places exactly as `int[8]` is. The
  // old gate demanded an INTEGER element and rejected all three for a
  // reason that never applied to them.
  //
  // A parameter of array type cannot occur in C (the type decays to a
  // pointer, and `bindOrdinaryParam` has no ArrayType arm), so the place
  // claim is a local's claim: require one.
  if (astContext().getAsConstantArrayType(type))
    return llvm::isa<clang::VarDecl>(var) && !llvm::isa<clang::ParmVarDecl>(var);
  // A complete struct or union maps to `!emitrust.struct` (a union imports
  // as a one-field struct, a u8-only aggregate as a byte-region
  // `!emitrust.array`) and takes the same `isAggregate` place branch; a
  // by-value struct parameter takes `bindOrdinaryParam`'s StructType arm
  // independently. Records in namespace `std` are EXCLUDED: `mapType`
  // diverts them to `mapStdLibraryType`, and a literal-initialized
  // `std::string_view` local decomposes into i64 cursor CELLS instead of a
  // place -- exactly the hazard this predicate exists to keep out.
  if (const auto *record = type->getAsRecordDecl()) {
    if (!record->getDefinition())
      return false;
    if (record->isInStdNamespace())
      return false;
    return true;
  }
  // Floats, decomposed pointers (i64 cursor cells), and anything else:
  // reject to the CFG `while` lowering. A float SCALAR is genuinely
  // cell-backed; no corpus loop is blocked by one.
  return false;
}

// FR-61f-c slice 1. A pointer is the one body-variable class that is
// deliberately NOT routed to a place: it decomposes into i64/i1/i32 entry-block
// CELLS (`PointerLocalInfo`), and design.md's prescription -- convert those
// cells into `emitrust.variable` places -- was measured WORSE than the
// alternative taken here. A place is opaque to `canonicalize`, so the
// converted form renders a dead `let` plus unfolded cursor arithmetic; the
// same head-improves/body-degrades shape that made the FR-61f (d) `?:` slice
// net negative.
//
// The mechanism used instead is HOISTING: load the cells ONCE, in the
// enclosing block, and use the SSA value inside the region. `canonicalize`
// then folds the `+ 0` away, so the head AND the body improve. That is only
// sound when the pointer's state cannot change across iterations, which is
// exactly what this predicate proves.
//
// It is stated on the clang AST alone -- no `pointerLocals`, no `regionOf` --
// for a hard ordering reason: `matchRangeFor` runs once from
// `collectRangeForPlaceScalars`, which every function-emission entry calls
// BEFORE `pointerRegions.analyze`, so `pointerLocals` is empty there. A
// predicate that consulted it would answer differently in the pre-pass than at
// emission and desynchronize `placeBackedScalars` from the lift decision. It
// also costs nothing: 125 of the 161 corpus sites blocked by a pointer body
// var have no `PointerRegion` at all (a read-only cursor PARAMETER walked by
// index is never *bound*, so the region model never sees it), and gating on
// the region model yields +12 where body-invariance yields +142.
bool CImporter::rangeForBodyPointerIsHoistable(const clang::VarDecl *var,
                                               const clang::Stmt *body) {
  clang::QualType type = var->getType().getCanonicalType();
  if (!type->isPointerType())
    return false;
  // A function pointer is already a place (`!emitrust.fn_ptr`) and is handled
  // by `rangeForBodyVarIsPlaceBacked`; nothing to hoist.
  if (type->isFunctionPointerType())
    return false;
  // A pointer-to-pointer LOCAL lives in `pointerPointerLocals`, a separate
  // registry with its own cell that `emitRangeFor` cannot reach through
  // `pointerLocals`. Admitting one would lift the loop and then leave its cell
  // read inside the region -- the located `memref.alloca` legalization error,
  // not a miscompile, but a regression against the working `while` lowering it
  // has today. `pointers-ptr-to-ptr.c` is the corpus instance. A `T **`
  // PARAMETER is a different animal: `emitPointerPointerLocal` is the only
  // writer of `pointerPointerLocals` and it runs off a `DeclStmt`, so a
  // parameter (`char **argv`) decomposes through the ordinary `pointerLocals`
  // cursor cell and hoists like any other.
  if (type->getPointeeType().getCanonicalType()->isPointerType() &&
      !llvm::isa<clang::ParmVarDecl>(var))
    return false;
  // A pointer DECLARED INSIDE the body has its initializing store inside the
  // region, so there is no value to load before it. This is the FR-61f-c
  // slice-3 residue (~19 sites across four corpora); it keeps the `while`
  // lowering it works under today.
  if (stmtDeclaresVar(body, var))
    return false;
  // The invariance proof. `stmtWritesVar` RECURSES, so a mutation in the inner
  // loop of a nested pair refuses the outer lift too; `addressTaken` is
  // function-wide, so it fences every write reached through a callee (`&p`
  // escaping) and every second-order write (`*pp = q`, pointers-ptr-to-ptr.c).
  //
  // This clause is also the FENCE for a fourth `blocksRangeForLift` leak.
  // `blocksRangeForLift` is a statement-shape blocklist and cannot see that a
  // pointer REBINDING (nullable / multi-base dispatch) emits cf blocks inside
  // the region; admitting a mutated pointer segfaults
  // `mlir::OpTrait::impl::verifyNSuccessors` on `compound-literals.c`,
  // `pointers-member-base.c` and `pointers-multi-base.c` -- measured: zero
  // crashes over 460 EndToEnd files at this gate, three the moment mutation is
  // admitted. Any later widening must fix that leak FIRST.
  return !stmtWritesVar(body, var) && !addressTaken.contains(var);
}

/// True when `bound` can be carried at the induction's own type `type`
/// WITHOUT narrowing: it is either an integer constant representable there,
/// or an expression already of exactly that type.
bool CImporter::rangeForBoundFitsType(const clang::Expr *bound,
                                      clang::QualType type) {
  unsigned bits = astContext().getIntWidth(type);
  bool isUnsigned = type->isUnsignedIntegerType();
  if (std::optional<llvm::APSInt> constant =
          bound->getIntegerConstantExpr(astContext())) {
    if (isUnsigned)
      return !constant->isNegative() && constant->getActiveBits() <= bits;
    return constant->getSignificantBits() <= bits;
  }
  return bound->IgnoreParenImpCasts()->getType().getCanonicalType() ==
         type.getCanonicalType();
}

/// FR-61f-7, clause 1b: whether the loop may be emitted at the INDUCTION'S
/// OWN type rather than the historical i32.
///
/// This was recorded as "a design decision needing an op-signature change".
/// It is not: `AllTypesMatch<["lowerBound","upperBound","step"]>` says only
/// that the three operands agree WITH EACH OTHER, the operand constraint is
/// already `AnyTypeOf<[AnyInteger, Index]>`, and `emitFor` is type-agnostic.
/// The i32-ness was purely `emitRangeFor` hardcoding `getI32Type()`.
///
/// `int` keeps exactly its historical acceptance -- the status quo that 823
/// tests already pin -- so this predicate only ever ADDS shapes. For every
/// other type it is deliberately narrow, because emitting at a narrower or
/// unsigned type opens THREE divergences from C that a cast-and-hope
/// implementation would walk straight into:
///
///  (1) TRUNCATION. A runtime `size_t`/`unsigned long` bound above INT_MAX
///      cast down to i32 changes the trip count outright
///      (test/EndToEnd/borrow-bundle-scalarize.c:118 is that shape). Fixed at
///      the root: the bound is never narrowed, it is required to fit.
///
///  (2) INTEGER PROMOTION. In C, `i < HI` on a narrow `i` compares at `int`,
///      not at `i`'s type -- which is why `for (uint8_t i = 0; i < 300; i++)`
///      NEVER TERMINATES in C while a u8 range would. Fixed by refusing any
///      type narrower than `int`: at rank >= int there is no promotion, so
///      the C comparison and the Rust range agree by construction.
///
///  (3) DEFINED WRAPAROUND. Signed overflow is UB, which is what let 61f-3
///      refine `i <= INT_MAX` into a cleanly-terminating `..=`. UNSIGNED
///      wraparound is DEFINED, so the same move would be a behaviour change:
///      `for (unsigned i = 0; i <= UINT_MAX; i++)` loops forever in C. So an
///      unsigned induction must additionally prove the increment PAST the
///      last iteration cannot wrap.
bool CImporter::rangeForInductionTypeOk(const clang::VarDecl *iv,
                                        const clang::Expr *lo,
                                        const clang::Expr *hi, int64_t step,
                                        bool inclusive) {
  clang::QualType type = iv->getType().getCanonicalType();
  // Exactly the historical acceptance, bit for bit: `int` inductions keep
  // today's behaviour including today's bound casting, so this widening is
  // additive and every existing lift is byte-inert.
  if (type == astContext().IntTy)
    return true;
  // Enums map to `!emitrust.enum` and `_Bool` to i1; neither is a counter.
  if (!type->isIntegerType() || type->isBooleanType() ||
      type->isEnumeralType())
    return false;
  // Divergence (1): both bounds and the step must be carried at `type`
  // without narrowing.
  if (!rangeForBoundFitsType(lo, type) || !rangeForBoundFitsType(hi, type))
    return false;
  unsigned bits = astContext().getIntWidth(type);
  if (step <= 0)
    return false;
  // Divergence (2), the PROMOTION case: a type of lower rank than `int`.
  //
  // 61f-7 refused these outright, on the type's rank, and said so: "it costs
  // the `uint8_t` (11) and `uint16_t`/`unsigned short` (16) buckets; ZERO
  // corpus loops are blocked only by it". The first half was right and the
  // second was measured wrong later -- the induction-type clause is 34 unique
  // sites, the second-largest remainder there is. So the fence is refined
  // here rather than kept, and refined rather than dropped, because the
  // hazard it named is real: in C, `i < HI` on a narrow `i` compares at
  // `int`, which is why `for (uint8_t i = 0; i < 300; i++)` NEVER TERMINATES
  // while a u8 range stops at 255.
  //
  // The exact condition. Promotion is harmless whenever every value the loop
  // can produce is inside T's range, because then the comparison at `int` and
  // the comparison at T agree on every iteration and no conversion back to T
  // is ever out of range. Two obligations give that:
  //
  //   - HI must be a COMPILE-TIME CONSTANT. A runtime bound is unbounded from
  //     the matcher's view even when its TYPE is T: a `uint8_t` HI may be 255,
  //     and then C's final `i += 1` wraps to 0 and loops forever where Rust's
  //     `0..255` simply ends. `rangeForBoundFitsType` accepts an
  //     expression-already-of-type-T, which is sound for a wide type and is
  //     precisely the trap for a narrow one -- so narrow types need the
  //     stricter test, not the shared one.
  //   - HI + step must fit T, so the increment PAST the last iteration cannot
  //     leave the range. That is the same obligation the unsigned arm below
  //     carries, applied here to signed narrow types too: converting an
  //     out-of-range value to a narrow signed type is implementation-defined,
  //     not the plain UB that let 61f-3 refine `..=`, so it earns no
  //     exemption.
  //
  // LO may still be a runtime expression of type T: its value is in range by
  // construction, and a LO past HI just means the loop does not run.
  if (astContext().getTypeSize(type) <
      astContext().getTypeSize(astContext().IntTy)) {
    std::optional<llvm::APSInt> bound = hi->getIntegerConstantExpr(astContext());
    if (!bound || bound->isNegative())
      return false;
    bool isUnsigned = type->isUnsignedIntegerType();
    uint64_t typeMax = isUnsigned ? ((uint64_t(1) << bits) - 1)
                                  : ((uint64_t(1) << (bits - 1)) - 1);
    if (static_cast<uint64_t>(step) > typeMax)
      return false;
    return bound->getZExtValue() <= typeMax - static_cast<uint64_t>(step);
  }
  if (!type->isUnsignedIntegerType())
    return static_cast<uint64_t>(step) < (uint64_t(1) << (bits - 1));
  // Divergence (3), unsigned only. The loop's LAST act is the increment that
  // carries the induction past the bound; if THAT wraps, C restarts and Rust
  // stops.
  //   - a constant bound C is safe iff C + step still fits;
  //   - a runtime bound of the induction's own type is safe only for the
  //     half-open unit-step shape, where the final value is exactly the bound
  //     and the bound is a value of the type by construction.
  uint64_t max = bits >= 64 ? std::numeric_limits<uint64_t>::max()
                            : ((uint64_t(1) << bits) - 1);
  if (static_cast<uint64_t>(step) > max)
    return false;
  if (std::optional<llvm::APSInt> constant =
          hi->getIntegerConstantExpr(astContext())) {
    uint64_t bound = constant->getZExtValue();
    return bound <= max - static_cast<uint64_t>(step);
  }
  return step == 1 && !inclusive;
}

std::optional<RangeFor>
CImporter::matchRangeFor(const clang::ForStmt *stmt) {
  const clang::Stmt *init = stmt->getInit();
  const clang::Expr *cond = stmt->getCond();
  const clang::Expr *inc = stmt->getInc();
  const clang::Stmt *body = stmt->getBody();
  if (!init || !cond || !inc || !body)
    return std::nullopt;

  // Clause 1 (+6): the init supplies the induction and LO. Two forms:
  //   (a) the DeclStmt form `int i = LO;` -- `i` is loop-scoped in C, so it
  //       cannot be referenced after the loop and clause 6 holds for free.
  //   (b) FR-61f-5, the assignment form `i = LO;` with `i` declared outside.
  //       `for i in LO..HI` binds `i` LOOP-SCOPED in Rust, so this form is
  //       behaviour-neutral only when `i` is provably never read again after
  //       the loop; `inductionDeadAfter` proves that or refuses.
  const clang::VarDecl *iv = nullptr;
  const clang::Expr *lo = nullptr;
  if (const auto *declStmt = llvm::dyn_cast<clang::DeclStmt>(init)) {
    if (!declStmt->isSingleDecl())
      return std::nullopt;
    iv = llvm::dyn_cast<clang::VarDecl>(declStmt->getSingleDecl());
    if (!iv || !iv->isLocalVarDecl())
      return std::nullopt;
    lo = iv->getInit();
  } else if (const auto *initExpr = llvm::dyn_cast<clang::Expr>(init)) {
    const auto *assign = llvm::dyn_cast<clang::BinaryOperator>(
        initExpr->IgnoreParenImpCasts());
    if (!assign || assign->getOpcode() != clang::BO_Assign)
      return std::nullopt;
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(
        assign->getLHS()->IgnoreParenImpCasts());
    if (!ref)
      return std::nullopt;
    iv = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
    // Automatic-storage local only: a global, a function-local `static` and
    // a parameter all fail one of these two and keep the `while` lowering.
    if (!iv || !iv->isLocalVarDecl() || !iv->hasLocalStorage())
      return std::nullopt;
    lo = assign->getRHS();
    // LO must not read the induction: `emitRangeFor` evaluates LO before the
    // loop, where a stale `inductionValues` entry from an earlier lifted
    // loop over the same decl would otherwise be consulted.
    if (readsVar(lo, iv))
      return std::nullopt;
    if (!inductionDeadAfter(currentFunctionBody, stmt, iv))
      return std::nullopt;
  } else {
    return std::nullopt;
  }
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

  // Clause 1b (FR-61f-7): the induction's TYPE. It used to have to be exactly
  // `int`; it now only has to be one the ForOp can carry losslessly. The
  // check lives here, after clauses 2 and 3, because its soundness depends on
  // the step and on whether the range is inclusive.
  if (!rangeForInductionTypeOk(iv, lo, hi, step, inclusive))
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
    // FR-61f-8 DEFECT FIX. `stmtWritesVar` is a SYNTACTIC walk over the body,
    // so on its own it proves nothing about a write the body makes THROUGH A
    // CALL. That was a measured miscompile, not a theoretical one:
    //
    //   int limit = 5;
    //   void shrink(void) { if (limit > 2) limit--; }
    //   for (i = 0; i < limit; i++) { s += i; shrink(); }
    //
    // lifted to `for i in 0..limit`, which snapshots the bound and runs 5
    // times where C re-reads it and runs 3. The crate COMPILED CLEAN; only
    // the byte-diff oracle saw it (603 from clang, 1505 from the crate).
    //
    // So once the body can call anything, the bound must be proved
    // UNREACHABLE to the callee, not merely unwritten in the body's own text.
    // The proof admitted here is the narrowest one that keeps the common
    // `for (i = 0; i < n; i++) { printf(..); }` shape: every variable the
    // bound reads is an automatic-storage, non-address-taken INTEGER. Such a
    // variable has no name and no address outside this frame, so no callee
    // can reach it. Anything else -- a global, a `static`, an address-taken
    // local, or any indirection at all (`*p`, `a[k]`, `s.n`, whose bases are
    // pointer/array/record and so not integers) -- refuses to the `while`
    // lowering, which re-reads the bound every iteration and is therefore
    // correct for all of them.
    bool bodyCalls = stmtContainsCall(body);
    for (const clang::VarDecl *var : hiVars) {
      if (var == iv || stmtWritesVar(body, var))
        return std::nullopt;
      if (bodyCalls &&
          (!var->hasLocalStorage() || addressTaken.contains(var) ||
           !var->getType().getCanonicalType()->isIntegerType()))
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
  // rejects the leftover alloca (spike 61f-0). So the whitelist admits
  // exactly the automatic-storage variables that reach an
  // `emitrust.variable` PLACE instead of a cell, and
  // `rangeForBodyVarIsPlaceBacked` is that predicate, stated on the clang
  // type so the matcher never has to run `mapType` (which imports records
  // and would shift struct_def emission order).
  // Everything else -- decomposed pointers (i64 cursor cells), floats,
  // `va_list`s -- rejects to the CFG `while` lowering. Deliberately narrow: a
  // green suite with partial coverage beats a broad matcher that miscompiles.
  llvm::SmallPtrSet<const clang::VarDecl *, 8> bodyVars;
  collectRefVars(body, bodyVars);
  llvm::SmallVector<const clang::VarDecl *, 4> hoistPointers;
  for (const clang::VarDecl *var : bodyVars) {
    if (var == iv)
      continue;
    // FR-61f-10: a global or a function-local `static` is admitted too, and
    // for the same reason the aggregates were -- the cell story never applied
    // to it. A global is not a frame slot at all: a read is an
    // `emitrust.global_load` and a write an `emitrust.global_store`, both
    // plain SSA ops that sit inside a region as happily as anywhere else, and
    // no `memref.alloca` is involved on any arm. A function-local `static` is
    // module-level state that `emitLocalVar` routes to `createGlobal` under a
    // `<function>_<name>` mangling, so it is the same op pair.
    //
    // The one exception is an ADDRESS-TAKEN global. `&g` pulls in the
    // `emitrust.global_addr` / `emitrust.global_cells` machinery (the FR-80
    // requirement-global path), which stages real cells; that stays out.
    if (!var->hasLocalStorage() && addressTaken.contains(var))
      return std::nullopt;
    // FR-61f-c slice 1: a BODY-INVARIANT pointer is admitted without being a
    // place. Its cells are hoisted to SSA values in front of the region by
    // `emitRangeFor`; see `rangeForBodyPointerIsHoistable`.
    if (rangeForBodyPointerIsHoistable(var, body)) {
      hoistPointers.push_back(var);
      continue;
    }
    if (!rangeForBodyVarIsPlaceBacked(var))
      return std::nullopt;
  }
  // `bodyVars` is a pointer-keyed set, so its iteration order is the
  // allocator's. Sort the hoist list into source order: it drives the order of
  // the emitted `memref.load`s, and an unstable one would make the emitted
  // crate non-reproducible.
  llvm::sort(hoistPointers,
             [](const clang::VarDecl *a, const clang::VarDecl *b) {
               return a->getLocation().getRawEncoding() <
                      b->getLocation().getRawEncoding();
             });

  return RangeFor{iv, lo, hi, step, inclusive, hoistPointers};
}

LogicalResult CImporter::emitRangeFor(const RangeFor &range,
                                      const clang::ForStmt *stmt) {
  Location loc = translateLoc(stmt->getForLoc());
  // FR-61f-7: the ForOp carries the INDUCTION'S OWN type. `AllTypesMatch`
  // only ties the three operands to each other and the operand constraint
  // already admits any integer width and signedness, so nothing about the op
  // had to change -- this line was the whole i32 restriction.
  // `matchRangeFor`'s clause 1b guarantees the bounds reach this type without
  // narrowing, so the coercions below only ever widen a literal.
  FailureOr<Type> inductionType = mapType(range.iv->getType(), loc);
  if (failed(inductionType))
    return failure();
  Type intType = *inductionType;

  // Bounds are evaluated once, in the current block, before the loop.
  //
  // On the non-`int` path a CONSTANT bound is materialized directly at the
  // induction's type rather than emitted at its own and cast: `emitRValue`
  // gives an `int` literal an i32, and casting that would render the head as
  // `for i in 0i32 as u32..n` instead of `for i in 0u32..n`. Clause 1b
  // already proved the constant is representable, so this is a spelling
  // choice, not a value change. The `int` path deliberately does NOT take the
  // shortcut: it must stay byte-identical to what 823 tests already pin.
  bool ownType = range.iv->getType().getCanonicalType() != astContext().IntTy;
  auto emitBound = [&](const clang::Expr *expr) -> FailureOr<Value> {
    if (ownType)
      if (std::optional<llvm::APSInt> constant =
              expr->getIntegerConstantExpr(astContext()))
        return createScalarIntConstant(loc, intType,
                                       constant->getExtValue());
    return emitRValue(expr);
  };
  FailureOr<Value> lo = emitBound(range.lo);
  if (failed(lo))
    return failure();
  FailureOr<Value> hi = emitBound(range.hi);
  if (failed(hi))
    return failure();
  Value loValue = *lo;
  Value hiValue = *hi;
  // Coerce any differently-typed bound to the induction's type so the ForOp's
  // three operands share one (AllTypesMatch). For an `int` induction this is
  // the historical i32 coercion, unchanged; for any other it can only be the
  // widening of a literal, because clause 1b already refused every bound that
  // would have to narrow.
  if (loValue.getType() != intType)
    loValue = builder.create<emitrust::CastOp>(loc, intType, loValue);
  if (hiValue.getType() != intType)
    hiValue = builder.create<emitrust::CastOp>(loc, intType, hiValue);
  // An unsigned step must be an `emitrust.constant`: `arith.constant`
  // requires a signless type.
  Value stepValue = createScalarIntConstant(loc, intType, range.step);

  // FR-61f-c slice 1: HOIST the decomposition cells of every body-invariant
  // pointer the matcher admitted, loading each ONCE here -- in the enclosing
  // block, where mem2reg can still see the whole live range of the slot -- and
  // registering the loaded SSA value in `hoistedCells` so that `loadPlace`
  // returns it instead of emitting a `memref.load` inside the region. Without
  // this the loop lifts and then fails legalization: mem2reg will not promote
  // an alloca whose uses live in a nested region unless the parent implements
  // `PromotableRegionOpInterface`, and `emitrust.for` implements none.
  //
  // ALL THREE cells go together (cursor, nullable discriminant, enum-of-bases
  // discriminant): every one of them is read through the single `loadPlace`
  // seam, so a nullable pointer's `assert!` and a multi-base pointer's dispatch
  // come free. Any cell that stays a `memref.load` -- because the load is dead
  // here, say -- is DCE'd after mem2reg promotes the slot.
  //
  // `pointerLocals` may legitimately have no entry (a pointer whose region
  // analysis declined it). Skipping it is safe in the LOUD direction: the read
  // stays a `memref.load`, the alloca survives, and `convert-to-emitrust`
  // emits its existing located error. Incompleteness never becomes a wrong
  // answer.
  //
  // The set is TRANSITIVE over `PointerLocalInfo::base`. A member-array decay
  // (`int16_t *const ring = w->ring;`) subscripts the ROOT's element run at the
  // root's own cursor on every use (ImportC.cpp's root-cursor path), so reading
  // the decayed local inside the region also reads `w`'s cell -- and `w` need
  // not appear in the body at all, so the matcher never saw it. Each root is
  // re-proved invariant on its own terms before it is hoisted; a root that
  // fails is simply not hoisted, and the located legalization error stands.
  llvm::SmallVector<Value, 6> hoistedHere;
  llvm::SmallPtrSet<const clang::VarDecl *, 8> visited;
  llvm::SmallVector<const clang::VarDecl *, 8> worklist(
      range.hoistPointers.begin(), range.hoistPointers.end());
  while (!worklist.empty()) {
    const clang::VarDecl *var = worklist.pop_back_val();
    if (!visited.insert(var).second)
      continue;
    auto info = pointerLocals.find(var);
    if (info == pointerLocals.end())
      continue;
    for (Value cell : {info->second.cursorCell, info->second.nonNullCell,
                       info->second.baseIndexCell}) {
      // An outer lifted `for` may already have hoisted this cell; its value
      // dominates this block too, so reuse it and leave the outer loop owning
      // the restore.
      if (!cell || !llvm::isa<MemRefType>(cell.getType()) ||
          hoistedCells.contains(cell))
        continue;
      hoistedCells[cell] =
          builder.create<memref::LoadOp>(loc, cell, ValueRange()).getResult();
      hoistedHere.push_back(cell);
    }
    if (const clang::VarDecl *base = info->second.base)
      if (base->getType().getCanonicalType()->isPointerType() &&
          !stmtWritesVar(stmt->getBody(), base) &&
          !addressTaken.contains(base))
        worklist.push_back(base);
  }
  // Every entry maps a `Value` of the function being emitted. It must be gone
  // before this call returns on ANY path -- success, body failure, or the
  // staleness error below. Under `--recover` the failed function's ops are
  // ERASED, so a surviving entry is a dangling `Value` the next function's
  // first `loadPlace` would hand out: a measured segfault on `antirez/sds.c`,
  // not a theoretical one.
  auto dropHoists = [&] {
    for (Value cell : hoistedHere)
      hoistedCells.erase(cell);
  };

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
  // this induction) and is RESTORED on the way out.
  //
  // FR-61f-5: the restore is load-bearing for the assignment form, where the
  // same `VarDecl` outlives the loop. The matcher proves the induction is
  // dead after the loop, which means every later read is dominated by a
  // redefinition (`i = 0; while (i < n) ...`) -- and that redefinition
  // stores to the variable's PLACE. Leaving the block argument mapped made
  // those reads pick up the region-local SSA value instead; the spike hit
  // exactly that as `error: operand #0 does not dominate this use`.
  std::optional<Value> savedInduction;
  if (auto existing = inductionValues.find(range.iv);
      existing != inductionValues.end())
    savedInduction = existing->second;
  inductionValues[range.iv] = bodyBlock->getArgument(0);

  // FR-61f-12: the insertion point is now inside the `emitrust.for` region, so
  // `emitIfStmt` must take its structured arm. A COUNTER, not a flag: an `if`
  // inside a NESTED lifted `for` is a supported shape, so the inner loop's
  // decrement must not clear the outer loop's mode.
  ++liftedForDepth;
  LogicalResult bodyResult = emitStmt(stmt->getBody());
  --liftedForDepth;
  if (savedInduction)
    inductionValues[range.iv] = *savedInduction;
  else
    inductionValues.erase(range.iv);
  if (failed(bodyResult)) {
    dropHoists();
    return failure();
  }

  // The marker contract: a hoisted value is read once and reused for every
  // iteration, so a STORE to a hoisted cell inside the region would make it
  // stale -- the one way this mechanism could silently miscompile. The
  // matcher's invariance clause is supposed to make that unreachable; prove it
  // at emission rather than trust it, because the failure mode is a wrong
  // answer that compiles clean.
  bool stale = forOp.getRegion()
                   .walk([&](memref::StoreOp store) {
                     return hoistedCells.contains(store.getMemRef())
                                ? WalkResult::interrupt()
                                : WalkResult::advance();
                   })
                   .wasInterrupted();
  if (stale) {
    dropHoists();
    return emitError(loc) << "unsupported: loop-invariant pointer state is "
                             "written inside the lifted `for` body";
  }
  dropHoists();

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
        // The induction is materialized by `emitRangeFor` itself and globals
        // are already places. Everything else with automatic storage --
        // body-local integer scalars AND plain integer-scalar PARAMETERS
        // (which `bindOrdinaryParam` binds to a `memref.alloca` cell, not to
        // the raw block arg) -- would otherwise take the un-promotable cell
        // path inside the single-block region.
        if (var == range->iv || !var->hasLocalStorage() ||
            !var->getType()->isIntegerType())
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
  //
  // The partition is AST-ONLY -- no blocks are created here -- because
  // FR-198's ladder recognition below reads it and picks one of two
  // lowerings. Blocks are created inside whichever arm wins, in section
  // order, so the block layout of the non-ladder arm is unchanged.
  struct Section {
    SmallVector<const clang::Stmt *, 4> stmts;
  };
  SmallVector<Section> sections;
  SmallVector<llvm::APInt> caseValues;
  SmallVector<unsigned> caseSections;
  std::optional<unsigned> defaultSection;
  for (const clang::Stmt *child : body->body()) {
    const clang::Stmt *statement = child;
    if (llvm::isa<clang::SwitchCase>(child)) {
      sections.push_back({});
      while (const auto *label = llvm::dyn_cast<clang::SwitchCase>(statement)) {
        Location labelLoc = translateLoc(label->getKeywordLoc());
        if (const auto *caseStmt = llvm::dyn_cast<clang::CaseStmt>(label)) {
          if (caseStmt->getRHS())
            return emitError(labelLoc) << "unsupported: GNU case range";
          llvm::APSInt value =
              caseStmt->getLHS()->EvaluateKnownConstInt(astContext());
          caseValues.push_back(value.extOrTrunc(flagType.getWidth()));
          caseSections.push_back(sections.size() - 1);
        } else {
          defaultSection = sections.size() - 1;
        }
        statement = label->getSubStmt();
      }
    }
    // `isPlainSwitchBody` guaranteed the first child starts a label chain,
    // so `sections` is never empty here, and no label of this switch hides
    // inside `statement`.
    sections.back().stmts.push_back(statement);
  }

  // FR-198. Measure the longest run of consecutive sections that fall into
  // the next one. This is the quantity `lift-cf-to-scf` is quadratic in: it
  // structurizes a fall-through chain by duplicating the tail into every
  // arm, so case k of an N-chain is emitted about (N - k + 1) times.
  unsigned longestChain = 1;
  for (unsigned index = 0, run = 1; index + 1 < sections.size(); ++index) {
    if (switchSectionFallsThrough(sections[index].stmts)) {
      ++run;
      longestChain = std::max(longestChain, run);
    } else {
      run = 1;
    }
  }

  // FR-198. The LADDER shape: two or more sections, EVERY one of which falls
  // into the next (so `longestChain` spans the whole body), carrying nothing
  // that the guarded form cannot express.
  //
  // "Every section falls through" is what makes the body a ladder rather
  // than an ordinary switch: it is a shape clause, not a size threshold, and
  // there is deliberately no N-cutoff -- one construct gets one lowering, so
  // the small case the tests exercise is the same code path the large case
  // ships on. An ordinary `case k: ...; break;` switch, and the
  // `case k: return ...;` table (which falls through nowhere), keep the
  // `cf.switch` lowering below untouched.
  const clang::Stmt *ladderRefusal = nullptr;
  bool isLadder = sections.size() >= 2 && longestChain == sections.size();
  for (unsigned index = 0; isLadder && index < sections.size(); ++index) {
    for (auto [position, statement] : llvm::enumerate(sections[index].stmts)) {
      // HAZARD 1: a declaration at switch-body scope. C keeps it in scope for
      // every LATER case (C11 6.2.4p6 leaves it uninitialized when the
      // dispatch jumps over it), but each guarded `if` is its own Rust scope,
      // so a later section could not name it. Refused rather than hoisted:
      // hoisting would have to reproduce the jumped-over-initializer rule as
      // well, and the fallback lowering below already handles the shape
      // correctly.
      if (llvm::isa<clang::DeclStmt>(statement)) {
        ladderRefusal = statement;
        break;
      }
      // A `break` that is the FINAL top-level statement of the FINAL section
      // branches to the exit the section would fall out to anyway, so it is
      // a no-op and does not stop the chain. Every other `break` does.
      if (index + 1 == sections.size() &&
          position + 1 == sections[index].stmts.size() &&
          llvm::isa<clang::BreakStmt>(statement))
        continue;
      // HAZARDS 2 and 3: `break` targeting this switch, and any label or
      // `goto` (see `findLadderHazard`).
      if (const clang::Stmt *hazard = findLadderHazard(statement, true)) {
        ladderRefusal = hazard;
        break;
      }
    }
    if (ladderRefusal)
      isLadder = false;
  }

  if (isLadder) {
    // FR-198: the guarded linear form. `flag` -- the controlling expression,
    // already evaluated EXACTLY ONCE above -- selects an entry INDEX, and the
    // sections then run under `entry <= k` guards:
    //
    //   let entry = match flag { v0 => i0, ..., _ => D };
    //   if entry <= 0 { B0 } if entry <= 1 { B1 } ... if entry <= M-1 { BM-1 }
    //
    // where `D` is the index of the section carrying `default:` (which may be
    // anywhere in the body) or M when there is none, so an unmatched value
    // runs nothing. Indices are section positions, not case values, so sparse,
    // negative and out-of-order labels all work.
    //
    // Output is O(M): each section appears exactly once, and every guard is a
    // properly nested single-entry/single-exit diamond that `lift-cf-to-scf`
    // structurizes without duplicating anything.
    Type stepType = builder.getI32Type();
    SmallVector<Value> steps;
    for (unsigned index = 0; index <= sections.size(); ++index)
      steps.push_back(builder
                          .create<arith::ConstantOp>(
                              loc, builder.getIntegerAttr(stepType, index))
                          .getResult());
    llvm::ArrayRef<Value> stepRefs(steps);

    SmallVector<Block *> guardBlocks;
    SmallVector<Block *> bodyBlocks;
    for (unsigned index = 0; index < sections.size(); ++index) {
      guardBlocks.push_back(createBlock());
      bodyBlocks.push_back(createBlock());
    }
    Block *exitBlock = createBlock();
    // The entry index arrives as a block argument of the first guard, which
    // dominates every later guard and body, so no cell and no mem2reg round
    // trip is involved.
    Value entry = guardBlocks.front()->addArgument(stepType, loc);

    SmallVector<Block *> caseDestinations(caseValues.size(),
                                          guardBlocks.front());
    SmallVector<ValueRange> caseOperands;
    for (unsigned section : caseSections)
      caseOperands.push_back(stepRefs.slice(section, 1));
    builder.create<cf::SwitchOp>(
        loc, flag, guardBlocks.front(),
        stepRefs.slice(defaultSection.value_or(sections.size()), 1),
        llvm::ArrayRef<llvm::APInt>(caseValues), BlockRange(caseDestinations),
        llvm::ArrayRef<ValueRange>(caseOperands));

    // `break` targets the exit block -- only the admitted trailing one can
    // reach it -- and `continue` keeps targeting the latch of the enclosing
    // loop, so a `continue` or `return` inside a section abandons the rest of
    // the guard chain exactly as it abandons the rest of the switch in C.
    loopStack.push_back(
        {exitBlock,
         loopStack.empty() ? nullptr : loopStack.back().continueDest});
    for (unsigned index = 0; index < sections.size(); ++index) {
      Block *next =
          index + 1 < sections.size() ? guardBlocks[index + 1] : exitBlock;
      builder.setInsertionPointToEnd(guardBlocks[index]);
      Value taken = builder
                        .create<arith::CmpIOp>(loc, arith::CmpIPredicate::sle,
                                               entry, steps[index])
                        .getResult();
      builder.create<cf::CondBranchOp>(loc, taken, bodyBlocks[index],
                                       ValueRange(), next, ValueRange());
      builder.setInsertionPointToEnd(bodyBlocks[index]);
      LogicalResult sectionResult = success();
      for (const clang::Stmt *statement : sections[index].stmts)
        if (failed(sectionResult = emitStmt(statement)))
          break;
      if (failed(sectionResult)) {
        loopStack.pop_back();
        return failure();
      }
      if (!isTerminated(builder.getInsertionBlock()))
        builder.create<cf::BranchOp>(loc, next);
    }
    loopStack.pop_back();
    builder.setInsertionPointToEnd(exitBlock);
    return success();
  }

  // FR-198. The shape was not admitted, so the body falls to the duplicating
  // `cf.switch` lowering below. Past `kMaxSwitchFallThroughChain` that
  // lowering stops finishing at all (>360s and NO output and NO diagnostic at
  // a chain of 100), which is the one failure mode this repo forbids
  // outright, so refuse with a located diagnostic instead. The note names the
  // clause that cost the shape its linear lowering.
  if (longestChain > kMaxSwitchFallThroughChain) {
    InFlightDiagnostic diagnostic =
        emitError(loc) << "unsupported: switch with a fall-through chain of "
                       << longestChain << " cases (limit "
                       << kMaxSwitchFallThroughChain
                       << "); structurizing it duplicates the tail into every "
                          "arm and does not finish";
    if (ladderRefusal)
      diagnostic.attachNote(translateLoc(ladderRefusal->getBeginLoc()))
          << "this statement blocks the linear guarded lowering";
    return diagnostic;
  }

  SmallVector<Block *> sectionBlocks;
  for (unsigned index = 0; index < sections.size(); ++index)
    sectionBlocks.push_back(createBlock());
  SmallVector<Block *> caseBlocks;
  for (unsigned section : caseSections)
    caseBlocks.push_back(sectionBlocks[section]);
  Block *defaultBlock =
      defaultSection ? sectionBlocks[*defaultSection] : nullptr;

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
    builder.setInsertionPointToEnd(sectionBlocks[index]);
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
          index + 1 < sections.size() ? sectionBlocks[index + 1] : exitBlock;
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
  // FR-207. This lowering hands `lift-cf-to-scf` a fall-through ladder just
  // as the plain path's `cf.switch` fallback does, and it is quadratic in
  // exactly the same way -- FR-198 bounded the plain path and left this one
  // unbounded, so a chain nested one compound down (or inside a loop, as in
  // Duff's device) still reached the blowup with NO bound and NO diagnostic.
  // Re-measured here: 14300 lines / 1.11s at a chain of 32, 44236 / 9.72s at
  // 48, 100283 / 48.26s at 64, and past that no output and no diagnostic at
  // all. Refuse with a located diagnostic instead; the note says which clause
  // cost the body its structured lowering, since that is what a user would
  // have to change.
  unsigned longestChain = dispatchSwitchFallThroughChain(stmt);
  if (longestChain > kMaxSwitchFallThroughChain) {
    InFlightDiagnostic diagnostic =
        emitError(loc) << "unsupported: switch with a fall-through chain of "
                       << longestChain << " cases (limit "
                       << kMaxSwitchFallThroughChain
                       << "); structurizing it duplicates the tail into every "
                          "arm and does not finish";
    if (const auto *body = llvm::dyn_cast_if_present<clang::CompoundStmt>(
            stmt->getBody())) {
      bool precedesFirstLabel = false;
      if (const clang::Stmt *blocker =
              nonPlainSwitchBodyBlocker(body, precedesFirstLabel))
        diagnostic.attachNote(translateLoc(blocker->getBeginLoc()))
            << (precedesFirstLabel
                    ? "this statement precedes the first case label, so the "
                      "switch takes the duplicating dispatch lowering"
                    : "this label is nested inside a statement, so the switch "
                      "takes the duplicating dispatch lowering");
    }
    return diagnostic;
  }

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

const clang::CXXConstructExpr *
CImporter::admittedCopyConstructReturn(const clang::Expr *retValue) {
  const clang::Expr *inner = retValue->IgnoreParens();
  // A full expression with a materialized temporary wraps in
  // ExprWithCleanups; the copy's source is an lvalue, so nothing else
  // stands between the ReturnStmt and its construct node (measured on the
  // whole admitted table).
  if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(inner))
    inner = cleanups->getSubExpr()->IgnoreParens();
  const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(inner);
  if (!construct)
    return nullptr;
  const clang::CXXConstructorDecl *ctor = construct->getConstructor();
  if (!ctor || !ctor->isCopyConstructor() || !ctor->isUserProvided() ||
      !functions.lookup(cxxMethodMangledName(ctor)))
    return nullptr;
  return construct;
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
    } else if (currentParamCursorReturn) {
      // FR-104 parameter-cursor return: the returned pointer decomposes
      // exactly like the Stage-1 owner-index return above, but the proof
      // roots at ONE slice parameter and the cursor is RELATIVE to that
      // slice (a slice parameter's own coordinates start at 0, so the
      // decomposed cursor IS the return value). Value-preserving pointer
      // casts (`return (char *)s;`) are peeled exactly as the planner
      // peeled them. With the nullable lift, a null-constant site emits
      // the `None` literal and a cursor site wraps in `Some(...)`. The
      // checks below are defensive nets; `planParamCursorReturns` proved
      // every site.
      if (currentParamCursorReturnNullable &&
          isNullPointerConstantExpr(retValue)) {
        value = builder
                    .create<emitrust::LiteralOp>(
                        loc, currentReturnType, builder.getStringAttr("None"))
                    .getResult();
      } else {
        const clang::Expr *peeled = stripTrivia(retValue);
        while (const clang::Expr *sub = peelPointerCast(astContext(), peeled))
          peeled = stripTrivia(sub);
        FailureOr<PtrExprValue> pointer = emitPointerRValue(peeled);
        if (failed(pointer))
          return failure();
        if (pointer->base != currentParamCursorReturn || pointer->member ||
            pointer->baseIndex)
          return emitError(loc)
                 << "unsupported: returned pointer value (the return site "
                    "does not root in the proven parameter region)";
        if (pointer->nonNull) // Defensive; the plan's null sites are the
                              // null CONSTANTS, never a nullable region
                              // (resolveArgRoot excludes those).
          return emitError(loc)
                 << "unsupported: possibly-null pointer returned from a "
                    "parameter-cursor function";
        if (!pointer->cursor) // Defensive; a slice base always has one.
          return emitError(loc)
                 << "unsupported: the address of a scalar object cannot "
                    "be a parameter-cursor return";
        if (currentParamCursorReturnNullable)
          value = builder
                      .create<emitrust::CallOpaqueOp>(
                          loc, TypeRange{currentReturnType},
                          builder.getStringAttr("Some"),
                          /*args=*/ArrayAttr(), ValueRange{pointer->cursor})
                      .getResult(0);
        else
          value = pointer->cursor;
      }
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
               currentFamOptionPayload) {
      // FR-99: a NULLABLE owned FAM-record return. `planFamLift` pinned every
      // return site to a claimed owned local or to a NULL; a NULL inside an
      // elided malloc guard never reaches here at all, so every NULL that
      // does is REACHABLE and emits the `None` literal, and every owned
      // return moves the local out through `Some(...)`.
      if (retValue->isNullPointerConstant(
              astContext(), clang::Expr::NPC_NeverValueDependent) !=
          clang::Expr::NPCK_NotNull) {
        value = builder
                    .create<emitrust::LiteralOp>(
                        loc, currentReturnType, builder.getStringAttr("None"))
                    .getResult();
      } else {
        const clang::Expr *peeled = stripTrivia(retValue);
        while (const clang::Expr *sub = peelPointerCast(astContext(), peeled))
          peeled = stripTrivia(sub);
        const clang::VarDecl *owned = asLoadedLocalVarRef(peeled);
        Value ownedPlace = owned ? symbols.lookup(owned) : Value();
        if (!owned || !famAllocLocals.contains(owned) || !ownedPlace)
          return emitError(loc) << "unsupported: returned pointer value";
        Value moved = builder
                          .create<emitrust::LoadOp>(
                              loc, currentFamOptionPayload, ownedPlace)
                          .getResult();
        value = builder
                    .create<emitrust::CallOpaqueOp>(
                        loc, TypeRange{currentReturnType},
                        builder.getStringAttr("Some"),
                        /*args=*/ArrayAttr(), ValueRange{moved})
                    .getResult(0);
      }
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
    } else if (const clang::CXXConstructExpr *copyReturn =
                   admittedCopyConstructReturn(retValue)) {
      // W2.23: `return x;` through the ADMITTED user copy constructor.
      // Intercepted BEFORE the generic rvalue walk because the copy+dtor
      // class must be admitted HERE while its by-value ARGUMENT stays
      // refused there (emitRValue's droppy value-copy gate): the return
      // temp is moved out and never drops in the callee, so there is no
      // drop-point divergence a return can express -- unlike the
      // parameter temp, whose caller-vs-callee drop point measured
      // divergent (the spike's twocall probe).
      //
      // THE ONE IMPLEMENTATION-DEFINED SHAPE in the spike's elision table
      // is rejected here, on clang's own oracle: a non-null
      // `getNRVOCandidate()` marks exactly the single-named-local return,
      // which runs 0 copies by default and 1 under
      // -fno-elide-constructors on BOTH clang++ and g++ -- no emission is
      // byte-diff-clean against every conforming compiler. Every admitted
      // row (two-return functions, param returns) carries a NULL
      // candidate, measured.
      if (stmt->getNRVOCandidate())
        return emitError(loc)
               << "unsupported: NRVO-candidate return of a class with a "
                  "copy constructor";
      FailureOr<Type> structType = mapType(copyReturn->getType(), loc);
      if (failed(structType))
        return failure();
      Value copyPlace = createVariablePlace(loc, *structType, std::string());
      if (failed(emitCXXConstructInit(copyPlace, copyReturn, loc)))
        return failure();
      value = loadPlace(loc, copyPlace);
    } else {
      value = emitRValue(retValue);
    }
    if (failed(value))
      return failure();
    // W2.24: a can-throw-closure member's declared return value wraps in
    // the carrier's Ok0 variant (its signature already carries the enum).
    if (currentFunctionThrows) {
      if ((*value).getType() != currentThrowsOkType)
        return emitError(loc) << "unsupported: return value type mismatch";
      value = createThrowsValue(loc, /*isErr=*/false, *value);
    }
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
  // W2.24: `throw x;` / `throw;` in statement position, only under an
  // active throw plan — every other context (a throw-free-plan TU, a
  // value-position throw) keeps the generic located expression rejection.
  if (const auto *throwExpr = llvm::dyn_cast<clang::CXXThrowExpr>(e);
      throwExpr && throwsPlanActive)
    return emitThrowStmt(throwExpr);
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

/// W2.20: whether `e` is a `std::map` `operator[]` call — a SYNTACTIC
/// screen (the resolved operator method's parent record is `std::map`),
/// so it costs nothing and never triggers a type mapping.
static bool isStlMapSubscriptExpr(const clang::Expr *e) {
  const auto *opCall =
      llvm::dyn_cast<clang::CXXOperatorCallExpr>(e->IgnoreParenImpCasts());
  if (!opCall || opCall->getOperator() != clang::OO_Subscript)
    return false;
  const auto *method =
      llvm::dyn_cast_or_null<clang::CXXMethodDecl>(opCall->getDirectCallee());
  return method && method->getParent()->isInStdNamespace() &&
         method->getParent()->getIdentifier() &&
         method->getParent()->getName() == "map";
}

/// W2.20: whether `e` is a `std::map::at()` call. Its place is a SHARED
/// reference (`*std::ops::Index::index(&m, &k)`), so a store through it
/// would reach rustc as a deferred E0594 — the three write positions
/// reject it here instead. C++'s at() does return `V&`; a later wave
/// that wants the write form needs an `IndexMut` place, not a widening
/// of this one.
static bool isStlMapAtExpr(const clang::Expr *e) {
  const auto *memberCall =
      llvm::dyn_cast<clang::CXXMemberCallExpr>(e->IgnoreParenImpCasts());
  if (!memberCall)
    return false;
  const clang::CXXMethodDecl *method = memberCall->getMethodDecl();
  return method && method->getDeclName().isIdentifier() &&
         method->getName() == "at" && method->getParent()->isInStdNamespace() &&
         method->getParent()->getIdentifier() &&
         method->getParent()->getName() == "map";
}

LogicalResult CImporter::rejectStlMapAtWrite(const clang::Expr *lhs,
                                             Location loc) {
  if (!isStlMapAtExpr(lhs))
    return success();
  return emitError(loc) << "unsupported: std::map::at() is a read-only place; "
                           "assignment through it is not supported";
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
  if (failed(rejectStlMapAtWrite(op->getLHS(), loc)))
    return failure();
  // W2.20: `m[k] = rhs` MUST evaluate the right-hand side to a value
  // BEFORE the entry place is created, and this is a CORRECTNESS
  // requirement, not a style choice. Two independent reasons:
  //  * C++17 (P0145R3) sequences E2 before E1 in `E1 = E2`, and the map
  //    place has an OBSERVABLE side effect (a default insert). Measured
  //    on clang++ 21.1.8 and g++ 15: `std::map<int,int> a; a[1] =
  //    (int)a.size();` prints `a[1]==0` and `a.size()==1` — the size is
  //    read BEFORE the insert. The historical LHS-place-first order would
  //    print 1.
  //  * `BTreeMap::entry(&mut m, k)` holds a MUTABLE borrow of the map for
  //    as long as the place lives, so an LHS place built first and a
  //    right-hand side that touches the same map is rustc E0499
  //    ("cannot borrow `m` as mutable more than once at a time").
  //    Right-hand-side-first kills the read borrow under NLL before the
  //    write borrow is taken.
  if (isStlMapSubscriptExpr(op->getLHS())) {
    FailureOr<Value> value = emitRValue(op->getRHS());
    if (failed(value))
      return failure();
    FailureOr<Value> place = emitLValue(op->getLHS());
    if (failed(place))
      return failure();
    Value toStore = *value;
    if (auto lvalueType =
            llvm::dyn_cast<emitrust::LValueType>((*place).getType());
        lvalueType && lvalueType.getValueType() != toStore.getType()) {
      FailureOr<Value> converted =
          convertScalarValue(loc, toStore, lvalueType.getValueType());
      if (failed(converted))
        return failure();
      toStore = *converted;
    }
    if (failed(storeToPlace(loc, *place, toStore)))
      return failure();
    return place;
  }
  // W2.21: `*n = rhs` / `p->f = rhs` through a std::unique_ptr takes the
  // SAME mandatory right-hand-side-first order the map place above does,
  // for the second of its two reasons: the payload place is
  // `DerefMut::deref_mut(&mut p)`, a MUTABLE borrow of the Box held for as
  // long as the place lives, so an LHS place built first and a right-hand
  // side that reads the same Box (`p->id = p->get() + 1`) is rustc E0502.
  // Right-hand-side-first ends the read borrow under NLL before the write
  // borrow is taken. `stlBoxWriteContext` is what makes the place mutable
  // at all: a READ of a Box payload takes the shared `Deref::deref` borrow,
  // because two `&mut` borrows of one Box live at once (two field reads in
  // one printf) would be rustc E0499.
  if (isStlBoxWriteExpr(op->getLHS())) {
    FailureOr<Value> value = emitRValue(op->getRHS());
    if (failed(value))
      return failure();
    FailureOr<Value> place;
    {
      llvm::SaveAndRestore<bool> writing(stlBoxWriteContext, true);
      place = emitLValue(op->getLHS());
    }
    if (failed(place))
      return failure();
    Value toStore = *value;
    if (auto lvalueType =
            llvm::dyn_cast<emitrust::LValueType>((*place).getType());
        lvalueType && lvalueType.getValueType() != toStore.getType()) {
      FailureOr<Value> converted =
          convertScalarValue(loc, toStore, lvalueType.getValueType());
      if (failed(converted))
        return failure();
      toStore = *converted;
    }
    if (failed(storeToPlace(loc, *place, toStore)))
      return failure();
    return place;
  }
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
  if (failed(rejectStlMapAtWrite(op->getLHS(), loc)))
    return failure();
  // W2.20: `m[k] op= rhs` takes the same mandatory right-hand-side-first
  // order as the plain assignment above — C++17 sequences E2 before E1 in
  // `E1 op= E2` too, and the entry place's mutable borrow would otherwise
  // collide with any read of the same map on the right.
  // W2.21: `*n += rhs` / `p->f += rhs` through a std::unique_ptr takes the
  // same right-hand-side-first order and the same mutable payload borrow
  // as the plain assignment (see `emitAssignToPlace`); one `&mut` place
  // serves both the load and the store.
  if (isStlBoxWriteExpr(op->getLHS())) {
    FailureOr<Value> rhs = emitRValue(op->getRHS());
    if (failed(rhs))
      return failure();
    FailureOr<Value> place;
    {
      llvm::SaveAndRestore<bool> writing(stlBoxWriteContext, true);
      place = emitLValue(op->getLHS());
    }
    if (failed(place))
      return failure();
    Value current = loadPlace(loc, *place);
    FailureOr<Value> result =
        buildCompoundAssignValue(loc, op, current, *rhs);
    if (failed(result))
      return failure();
    if (failed(storeToPlace(loc, *place, *result)))
      return failure();
    return place;
  }
  if (isStlMapSubscriptExpr(op->getLHS())) {
    FailureOr<Value> rhs = emitRValue(op->getRHS());
    if (failed(rhs))
      return failure();
    FailureOr<Value> place = emitLValue(op->getLHS());
    if (failed(place))
      return failure();
    Value current = loadPlace(loc, *place);
    FailureOr<Value> result =
        buildCompoundAssignValue(loc, op, current, *rhs);
    if (failed(result))
      return failure();
    if (failed(storeToPlace(loc, *place, *result)))
      return failure();
    return place;
  }
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
    Location loc, const clang::CompoundAssignOperator *op, Value current,
    Value precomputedRhs) {
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
  // W2.20: the std::map subscript path evaluates the right-hand side
  // BEFORE the (mutably borrowing, default-inserting) entry place and
  // hands the value in here; every other caller emits it in place.
  Value rhsValue = precomputedRhs;
  if (!rhsValue) {
    FailureOr<Value> rhs = emitRValue(op->getRHS());
    if (failed(rhs))
      return failure();
    rhsValue = *rhs;
  }
  clang::BinaryOperatorKind opcode =
      clang::BinaryOperator::getOpForCompoundAssignment(op->getOpcode());
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
  if (failed(rejectStlMapAtWrite(op->getSubExpr(), loc)))
    return failure();
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
  // W2.21: `(*n)++` / `p->f++` through a std::unique_ptr needs the MUTABLE
  // payload borrow; a shared `Deref::deref` place would reach rustc as a
  // deferred E0594 instead.
  FailureOr<Value> place;
  {
    llvm::SaveAndRestore<bool> writing(stlBoxWriteContext,
                                       isStlBoxWriteExpr(op->getSubExpr()));
    place = emitLValue(op->getSubExpr(), &writeback);
  }
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
  // W2.22: a statement-position `std::cout`/`std::cerr` `<<` chain lowers to
  // the Rust print macros. Intercepted here, ahead of every by-name and
  // operator dispatch below: the chain is a `CXXOperatorCallExpr` whose
  // argument 0 is the NEXT chain link, which the ordinary operator path
  // would hand to `emitLValue` (there is no ostream place, hence today's
  // "unsupported assignable expression: CXXOperatorCallExpr"). Statement
  // position is the only position: the chain's ostream result has no
  // representation, so `emitCall`/`emitLValue` reject every value use.
  if (const auto *opCall = llvm::dyn_cast<clang::CXXOperatorCallExpr>(call)) {
    llvm::StringRef stream;
    llvm::SmallVector<const clang::CXXOperatorCallExpr *> links;
    if (matchOstreamChain(opCall, stream, links))
      return emitOstreamChain(opCall);
    // W2.23: whole-object `m = a;` through the IMPLICIT copy-assignment
    // operator lowers memberwise -- C++ runs operator=, NOT the copy
    // constructor, so the copy count at an assignment is 0 in every mode
    // (a `.clone()` here measured 1 against native 0, the spike's
    // miscompile probe). Statement position is the only position: the
    // `T&` result has no representation, so a value use keeps a located
    // rejection in `emitCall`. A USER `operator=` stays an FR-112
    // omission and falls through to the honest omitted-member wording.
    if (opCall->getOperator() == clang::OO_Equal) {
      const auto *method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(
          opCall->getDirectCallee());
      if (method && method->isImplicit() &&
          method->isCopyAssignmentOperator() &&
          !method->getParent()->isLambda() &&
          !method->getParent()->isInStdNamespace())
        return emitImplicitCopyAssign(opCall);
    }
  }
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
                               ValueRange operands, bool toStderr) {
  // A newline-terminated format folds its trailing `\n` into `println!`,
  // which writes byte-for-byte the same stdout as `print!` of the original
  // string (clippy::print_with_newline). A format without a trailing newline
  // keeps `print!`. W2.22's `std::cerr` chains select the stderr twins,
  // whose macro contract (and clippy lints) are identical.
  StringRef macro = toStderr ? "eprint!" : "print!";
  bool foldedNewline = false;
  if (!rustFormat.empty() && rustFormat.back() == '\n') {
    rustFormat.pop_back();
    macro = toStderr ? "eprintln!" : "println!";
    foldedNewline = true;
  }
  // A bare `println!()` (the whole format was a lone newline, no holes)
  // renders from an empty args array; `println!("")` would trip
  // clippy::println_empty_string.
  //
  // FR-131: the shortcut's PRECONDITION is the fold, not the emptiness. It
  // is sound only for the `ln` variants, whose macro still writes the
  // newline the fold consumed. A format that was ALREADY empty keeps the
  // non-`ln` macro, and `print!()` is not valid Rust -- rustc rejects it
  // with "requires at least a format string argument" and the whole crate
  // fails to build (`printf("")` used to emit exactly that). So an
  // unfolded empty format falls through to the normal path and keeps its
  // zero-length literal: `print!("")` compiles clean, writes zero bytes
  // (byte-identical to C's `printf("")`) and trips no clippy lint --
  // measured under clippy::all + clippy::pedantic, which has a
  // `println_empty_string` but no non-`ln` analogue.
  if (foldedNewline && rustFormat.empty() && operands.empty()) {
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
  // FR-191: both routings this function serves — a printf ALIAS and the
  // `fprintf(stdout, ...)` stdout swallow above — end in the very same
  // buffered-stdout `print!` as `emitPrintf`, so a `%s` over a char region
  // takes the same raw-bytes bypass here. (An alias whose target is not
  // printf-shaped never reaches this point: its argument 0 is not an
  // ordinary string literal and the check above rejects it.)
  bool rawBypassed = false;
  FailureOr<std::string> rustFormat = translatePrintfFormat(
      loc, call, literal, /*firstArgIndex=*/formatIndex + 1, operands,
      /*allowRawBypass=*/true, &rawBypassed);
  if (failed(rustFormat))
    return failure();

  // A trailing bypass consumed the whole format tail: no residual segment.
  if (rawBypassed && rustFormat->empty() && operands.empty())
    return success();

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
  // C99-43 C3 / FR-191: the stdout `print!` contexts are the only callers
  // that permit a `%s`/`%c` hole over a raw byte run (an argv argument, a
  // char region) to bypass the Latin-1 Display funnels — the format is
  // split into segments around the raw `*_out` helper calls.
  bool rawBypassed = false;
  FailureOr<std::string> rustFormat = translatePrintfFormat(
      loc, call, literal, /*firstArgIndex=*/1, operands,
      /*allowRawBypass=*/true, &rawBypassed);
  if (failed(rustFormat))
    return failure();

  // When a trailing bypass consumed the whole format tail, there is no
  // residual segment to print; emitting `print!("")` would be dead output.
  if (rawBypassed && rustFormat->empty() && operands.empty())
    return success();

  emitPrintMacro(loc, *rustFormat, operands);
  return success();
}

llvm::StringRef CImporter::stdOstreamGlobalName(const clang::Expr *expr) {
  const auto *ref =
      llvm::dyn_cast<clang::DeclRefExpr>(expr->IgnoreParenImpCasts());
  if (!ref)
    return {};
  const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
  if (!var || !var->isInStdNamespace() || !var->getDeclName().isIdentifier())
    return {};
  llvm::StringRef name = var->getName();
  if (name == "cout" || name == "cerr")
    return name;
  return {};
}

bool CImporter::matchOstreamChain(
    const clang::CXXOperatorCallExpr *call, llvm::StringRef &stream,
    llvm::SmallVectorImpl<const clang::CXXOperatorCallExpr *> &links) {
  links.clear();
  const clang::Expr *cursor = call;
  while (const auto *link =
             llvm::dyn_cast<clang::CXXOperatorCallExpr>(cursor->IgnoreParens())) {
    if (link->getOperator() != clang::OO_LessLess || link->getNumArgs() != 2)
      return false;
    links.push_back(link);
    cursor = link->getArg(0);
  }
  stream = stdOstreamGlobalName(cursor);
  if (stream.empty()) {
    links.clear();
    return false;
  }
  // The walk descended outermost-first; the operands are written in the
  // reverse order.
  std::reverse(links.begin(), links.end());
  return true;
}

LogicalResult
CImporter::emitOstreamChain(const clang::CXXOperatorCallExpr *call) {
  Location loc = translateLoc(call->getBeginLoc());
  llvm::StringRef stream;
  llvm::SmallVector<const clang::CXXOperatorCallExpr *> links;
  // Only reached through the `emitCallStmt` recognizer, which already
  // matched; the re-match keeps the flattening in one place.
  if (!matchOstreamChain(call, stream, links))
    return emitError(loc) << "unsupported: unrecognized std::ostream << chain";
  bool toStderr = stream == "cerr";
  llvm::StringRef byteHelper =
      toStderr ? "__emitrust_byte_err" : "__emitrust_byte_out";

  std::string rustFormat;
  llvm::SmallVector<Value> operands;
  // Flushes the accumulated format text and its operands as their own
  // macro call and starts a fresh segment. The same shape as
  // `translatePrintfFormat`'s argv-bypass `flushSegment`, and for the same
  // reason: a raw byte write has to be sequenced BETWEEN two format
  // segments, in program order.
  auto flushSegment = [&]() {
    if (rustFormat.empty() && operands.empty())
      return;
    emitPrintMacro(loc, rustFormat, operands, toStderr);
    rustFormat.clear();
    operands.clear();
  };
  // Appends literal bytes to the pending format segment under the same
  // guards `translatePrintfFormat` applies to a C format string: an
  // embedded NUL would print further than C++ does, a non-ASCII byte would
  // reach the generated Rust source verbatim and fail rustc's UTF-8 check,
  // and Rust's `{`/`}` need doubling.
  auto appendLiteral = [&](llvm::StringRef data,
                           Location dataLoc) -> LogicalResult {
    for (char c : data) {
      if (c == '\0')
        return emitError(dataLoc)
               << "unsupported: NUL byte in a std::ostream << string literal";
      if ((c < 0x20 || c > 0x7e) && c != '\n' && c != '\t' && c != '\r')
        return emitError(dataLoc)
               << "unsupported: non-printable or non-ASCII byte in a "
                  "std::ostream << string literal";
      if (c == '{') {
        rustFormat += "{{";
        continue;
      }
      if (c == '}') {
        rustFormat += "}}";
        continue;
      }
      rustFormat += c;
    }
    return success();
  };

  auto i32Type = builder.getIntegerType(32);
  auto stringType = emitrust::OpaqueType::get(builder.getContext(), "String");
  for (const clang::CXXOperatorCallExpr *link : links) {
    const clang::Expr *argExpr = link->getArg(1);
    Location argLoc = translateLoc(argExpr->getBeginLoc());
    // The operand's admissibility is decided by the RESOLVED overload's
    // PARAMETER type, never by the source expression's type: libstdc++
    // routes `unsigned short` to `operator<<(unsigned short)`, `size_t` to
    // `(unsigned long)`, and both an unscoped enum and `wchar_t` to
    // `(int)` — each of which prints differently from what the written
    // type suggests. Member overloads (int/double/bool/manipulator) carry
    // the value in parameter 0; the ADL FREE `std::operator<<` overloads
    // (`const char *`, the three char types, `std::string`) carry it in
    // parameter 1 after the stream.
    const clang::FunctionDecl *callee = link->getDirectCallee();
    unsigned paramIndex =
        llvm::isa_and_nonnull<clang::CXXMethodDecl>(callee) ? 0 : 1;
    if (!callee || callee->getNumParams() <= paramIndex)
      return emitError(argLoc)
             << "unsupported: a std::ostream << operand with no resolved "
                "operator<<";
    clang::QualType paramType = callee->getParamDecl(paramIndex)
                                    ->getType()
                                    .getNonReferenceType()
                                    .getUnqualifiedType();
    std::string paramName =
        paramType.getAsString(astContext().getPrintingPolicy());

    // C++17 sequences `E1 << E2` left to right and WRITES E1's output
    // before E2 is evaluated. Hoisting a side-effecting operand into a
    // `let` ahead of the pending `print!` would reorder that output — a
    // measured miscompile, not a theoretical one — so the segment is
    // flushed first and the operand evaluated after.
    if (argExpr->HasSideEffects(astContext()))
      flushSegment();

    // `std::endl` is the ONE manipulator this wave models: a newline plus a
    // flush, and the flush is unobservable (see `emitOstreamChain`'s
    // contract). It arrives as a `FunctionToPointerDecay` over a
    // `DeclRefExpr` naming the `endl` function TEMPLATE's specialization,
    // so the recognizer reads the decayed declaration's name. Every other
    // manipulator (`std::hex`, `std::flush`, `std::ends`, ...) keeps a
    // located rejection: none of them has a modeled image.
    if (paramType->isFunctionPointerType()) {
      const auto *ref =
          llvm::dyn_cast<clang::DeclRefExpr>(argExpr->IgnoreParenImpCasts());
      const auto *fn =
          ref ? llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()) : nullptr;
      if (fn && fn->isInStdNamespace() && fn->getDeclName().isIdentifier()) {
        if (fn->getName() == "endl") {
          rustFormat += '\n';
          continue;
        }
        return emitError(argLoc)
               << "unsupported: std::" << fn->getName()
               << " is not a recognized std::ostream manipulator";
      }
      return emitError(argLoc) << "unsupported: a std::ostream << operand of "
                                  "type '"
                               << paramName
                               << "' is not a recognized output type";
    }

    // A `const char *` operand. A string LITERAL folds straight into the
    // format string (inheriting the ASCII/NUL/brace guards above). Any
    // other `const char *` stays rejected this wave: printing it needs the
    // raw `__emitrust_cstr_out` byte funnel, since the Latin-1
    // `__emitrust_cstr` Display funnel double-encodes a runtime byte >=
    // 128 into two UTF-8 bytes where C++ writes one (measured).
    if (paramType->isPointerType()) {
      clang::QualType pointee = paramType->getPointeeType();
      if (pointee->isCharType()) {
        const clang::StringLiteral *literal =
            underlyingStringLiteral(argExpr->IgnoreParenImpCasts());
        if (!literal || !literal->isOrdinary())
          return emitError(argLoc)
                 << "unsupported: a 'const char *' std::ostream << operand "
                    "must be a string literal";
        if (failed(appendLiteral(literal->getString(), argLoc)))
          return failure();
        continue;
      }
      // `operator<<(const void *)` prints an ADDRESS, which the pointer
      // decomposition has compiled away and which no deterministic
      // byte-diff could reproduce anyway — the same policy `%p` keeps.
      return emitError(argLoc)
             << "unsupported: a pointer std::ostream << operand prints a "
                "nondeterministic address";
    }

    if (const auto *builtin = paramType->getAs<clang::BuiltinType>()) {
      switch (builtin->getKind()) {
      // The three CHARACTER overloads. libstdc++ writes ONE RAW BYTE for
      // each of `char`, `signed char` and `unsigned char` (the explicitly
      // signed variants are characters too, not numbers — measured), so
      // the operand goes through `__emitrust_byte_out`/`_err`, NOT the
      // `__emitrust_fmt_c` Display funnel whose `(x as u8) as char`
      // widening emits two UTF-8 bytes for 128..=255. A raw write is not a
      // format hole, so the pending segment flushes around it.
      case clang::BuiltinType::Char_S:
      case clang::BuiltinType::Char_U:
      case clang::BuiltinType::SChar:
      case clang::BuiltinType::UChar: {
        flushSegment();
        FailureOr<Value> value = emitRValue(argExpr);
        if (failed(value))
          return failure();
        auto valueIntType = llvm::dyn_cast<IntegerType>((*value).getType());
        if (!valueIntType)
          return emitError(argLoc)
                 << "unsupported: a std::ostream << operand of type '"
                 << paramName << "' is not a recognized output type";
        Value byte = castToIntType(argLoc, *value, builder.getIntegerType(8));
        if (toStderr)
          needsByteErrHelper = true;
        else
          needsByteOutHelper = true;
        builder.create<emitrust::CallOpaqueOp>(
            argLoc, TypeRange(), builder.getStringAttr(byteHelper),
            /*args=*/ArrayAttr(), ValueRange{byte});
        continue;
      }
      // `operator<<(float)` and `operator<<(double)` are `%.6g` by
      // construction (libstdc++ hands both to `_M_insert<double>` with the
      // default precision 6), which the project's C-compatible
      // `__emitrust_fmt_float(x, conv=2, prec=-1, ...)` reproduces byte for
      // byte — including `inf`/`-inf`/`nan`/`-0`/`1e+06`/`1e+300`, none of
      // which Rust's `{}`, `{:.6}` or `{:e}` gets right.
      case clang::BuiltinType::Float:
      case clang::BuiltinType::Double: {
        FailureOr<Value> value = emitRValue(argExpr);
        if (failed(value))
          return failure();
        Value widened = *value;
        if (!llvm::isa<Float64Type>(widened.getType())) {
          if (!llvm::isa<Float32Type>(widened.getType()))
            return emitError(argLoc)
                   << "unsupported: a std::ostream << operand of type '"
                   << paramName << "' is not a recognized output type";
          widened = builder
                        .create<arith::ExtFOp>(argLoc, builder.getF64Type(),
                                               widened)
                        .getResult();
        }
        needsFloatFormatExtHelper = true;
        Value formatted =
            builder
                .create<emitrust::CallOpaqueOp>(
                    argLoc, TypeRange{stringType},
                    builder.getStringAttr("__emitrust_fmt_float"),
                    /*args=*/ArrayAttr(),
                    ValueRange{widened, createIntConstant(argLoc, i32Type, 2),
                               createIntConstant(argLoc, i32Type, -1),
                               createIntConstant(argLoc, i32Type, 0),
                               createIntConstant(argLoc, i32Type, 0)})
                .getResult(0);
        operands.push_back(formatted);
        rustFormat += "{}";
        continue;
      }
      default:
        break;
      }
      // `operator<<(bool)` prints `1`/`0` in the default (non-`boolalpha`)
      // stream state, where Rust's `{}` on a `bool` prints `true`/`false`;
      // the zero-extension to `i32` is what makes the two agree.
      if (paramType->isBooleanType()) {
        FailureOr<Value> value = emitRValue(argExpr);
        if (failed(value))
          return failure();
        FailureOr<Value> widened =
            extendBool(argLoc, *value, astContext().IntTy);
        if (failed(widened))
          return failure();
        operands.push_back(*widened);
        rustFormat += "{}";
        continue;
      }
      // Every integer overload: Rust's `{}` is byte-identical to
      // libstdc++'s on all of them, at the extremes included (measured on
      // SHRT_MIN..ULLONG_MAX). The value is cast to the OVERLOAD's exact
      // width and signedness first, which is what makes an unscoped enum
      // (`operator<<(int)`) and a `size_t` (`(unsigned long)`) print what
      // C++ prints rather than what their written types suggest.
      if (paramType->isIntegerType()) {
        FailureOr<Value> value = emitRValue(argExpr);
        if (failed(value))
          return failure();
        if (!llvm::isa<IntegerType>((*value).getType()))
          return emitError(argLoc)
                 << "unsupported: a std::ostream << operand of type '"
                 << paramName << "' is not a recognized output type";
        unsigned bits = astContext().getTypeSize(paramType);
        IntegerType target =
            paramType->isSignedIntegerType()
                ? builder.getIntegerType(bits)
                : IntegerType::get(builder.getContext(), bits,
                                   IntegerType::Unsigned);
        operands.push_back(castToIntType(argLoc, *value, target));
        rustFormat += "{}";
        continue;
      }
    }

    // A `std::string` operand borrows its `String` place shared and prints
    // by `Display` — the same lowering, on the same place, that
    // `printf("%s", s.c_str())` already uses, so it inherits exactly the
    // existing string-content guarantees and adds no new exposure.
    if (const auto *record = paramType->getAsCXXRecordDecl();
        record && record->isInStdNamespace() &&
        record->getDeclName().isIdentifier() &&
        record->getName() == "basic_string") {
      FailureOr<Value> place = emitLValue(argExpr->IgnoreParenImpCasts());
      if (failed(place))
        return failure();
      auto lvalueType =
          llvm::dyn_cast<emitrust::LValueType>((*place).getType());
      auto opaque =
          lvalueType ? llvm::dyn_cast<emitrust::OpaqueType>(
                           lvalueType.getValueType())
                     : emitrust::OpaqueType();
      if (!opaque || opaque.getValue() != "String")
        return emitError(argLoc)
               << "unsupported: a std::ostream << operand of type '"
               << paramName << "' is not a recognized output type";
      operands.push_back(
          builder
              .create<emitrust::AddrOfOp>(
                  argLoc, emitrust::RefType::get(opaque), *place,
                  /*is_mut=*/false)
              .getResult());
      rustFormat += "{}";
      continue;
    }

    return emitError(argLoc)
           << "unsupported: a std::ostream << operand of type '" << paramName
           << "' is not a recognized output type";
  }
  flushSegment();
  return success();
}

FailureOr<std::string> CImporter::translatePrintfFormat(
    Location loc, const clang::CallExpr *call,
    const clang::StringLiteral *literal, unsigned firstArgIndex,
    SmallVectorImpl<Value> &operands, bool allowRawBypass,
    bool *rawBypassed) {
  // Translate the C format string into a Rust format string. The literal's
  // bytes already have C escapes decoded (a "\n" is a real newline byte);
  // the StringAttr printer re-escapes them for the textual assembly.
  llvm::StringRef format = literal->getString();
  std::string rustFormat;
  rustFormat.reserve(format.size());
  unsigned argIndex = firstArgIndex;
  // FR-192: a `%s` argument is CONVERTED -- the pointee read -- when printf
  // reaches the directive, not when the argument is evaluated. C17 6.5.2.2p10
  // places a sequence point before the call, and the array-to-pointer decay
  // of a buffer does NOT access the array's stored value, so a LATER argument
  // whose call writes that buffer is well defined and its store IS visible to
  // the conversion (`printf("[%s] %d\n", buf, bump(buf))` prints `[Bb] 1`,
  // both under clang and under gcc). Lowering the `%s` where it is written in
  // the format string snapshots the region BEFORE that store and lost it -- a
  // measured miscompile, native `5b42625d20310a` vs emitted `5b61625d20310a`.
  //
  // Such a `%s` is therefore DEFERRED: its operand takes a reserved slot in
  // `operands` and the ops that read the region are created only once every
  // later argument has been lowered. That is exactly C's order -- all side
  // effects complete, then the conversions read -- and it is also what Rust's
  // borrow rules want, because the shared borrow of the buffer is now created
  // AFTER the mutable borrow the intervening call needs, instead of spanning
  // it. (A `%d` argument is a different matter: its VALUE is read during
  // argument evaluation, so a later argument writing that object is an
  // unsequenced read/write conflict and plain UB. Only `%s` is reordered.)
  struct DeferredString {
    size_t slot;
    const clang::Expr *argExpr;
    std::optional<unsigned> precision;
    // Non-null for an `argv[i]` argument, which the generic `%s` shapes do
    // not admit: it reaches the format hole only through the argv table.
    const clang::Expr *argvIndexExpr;
  };
  SmallVector<DeferredString> deferredStrings;
  // Fills every reserved slot, in the order the directives appear. Called
  // immediately before anything consumes `operands` -- a segment flush or the
  // final `print!`/`format!` the caller builds -- and by then every argument
  // that could write one of these regions has already been lowered.
  auto materializeDeferredStrings = [&]() -> LogicalResult {
    for (const DeferredString &deferred : deferredStrings) {
      if (deferred.argvIndexExpr) {
        // An argv element: the same byte run the raw bypass would have
        // written, rendered instead through the Latin-1 `__emitrust_cstr`
        // funnel because the write has to happen at print time, not here.
        FailureOr<Value> slice =
            emitArgvArgSlice(loc, deferred.argvIndexExpr);
        if (failed(slice))
          return failure();
        auto stringType =
            emitrust::OpaqueType::get(builder.getContext(), "String");
        Value text;
        if (deferred.precision) {
          needsCStrNHelper = true;
          Value count =
              createIntConstant(loc, builder.getIntegerType(64),
                                static_cast<int64_t>(*deferred.precision));
          text = builder
                     .create<emitrust::CallOpaqueOp>(
                         loc, TypeRange{stringType},
                         builder.getStringAttr("__emitrust_cstr_n"),
                         /*args=*/ArrayAttr(), ValueRange{*slice, count})
                     .getResult(0);
        } else {
          needsCStrHelper = true;
          text = builder
                     .create<emitrust::CallOpaqueOp>(
                         loc, TypeRange{stringType},
                         builder.getStringAttr("__emitrust_cstr"),
                         /*args=*/ArrayAttr(), ValueRange{*slice})
                     .getResult(0);
        }
        operands[deferred.slot] = text;
        continue;
      }
      FailureOr<Value> text = emitPrintfStringArg(
          deferred.argExpr, deferred.precision, /*rawByteSlice=*/nullptr);
      if (failed(text))
        return failure();
      operands[deferred.slot] = *text;
    }
    deferredStrings.clear();
    return success();
  };
  // C99-43 C3: flushes the format accumulated so far as its own `print!`
  // call and starts a fresh segment, so a raw `*_out` helper call can be
  // sequenced in program order between two format segments. A no-op when the
  // pending segment is empty (avoids `print!("")`).
  auto flushSegment = [&]() -> LogicalResult {
    if (failed(materializeDeferredStrings()))
      return failure();
    if (rustFormat.empty() && operands.empty())
      return success();
    emitPrintMacro(loc, rustFormat, operands);
    rustFormat.clear();
    operands.clear();
    return success();
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

    // Does any argument AFTER this one have side effects? C evaluates every
    // argument, and completes every side effect, before printf writes a byte
    // or converts a directive (C17 6.5.2.2p10), which has two consequences
    // here and both are keyed off this one question:
    //   * FR-191: a raw-bytes bypass WRITES OUTPUT at argument-lowering time,
    //     which is before those effects run -- so the bypass is declined;
    //   * FR-192: a `%s` READS ITS REGION at conversion time, which is after
    //     those effects run -- so its materialization is deferred past them.
    bool laterArgSideEffects = false;
    for (unsigned k = argIndex, e = call->getNumArgs(); k < e; ++k)
      if (call->getArg(k)->HasSideEffects(astContext())) {
        laterArgSideEffects = true;
        break;
      }

    // C99-43 C3: an argv-fed `%s`/`%c` hole in the stdout `print!` context
    // bypasses the Latin-1 `__emitrust_cstr`/`__emitrust_fmt_c` Display
    // funnels (whose byte-to-char widening double-encodes non-ASCII argument
    // bytes) by flushing the pending format segment and writing the raw bytes
    // through the on-demand `*_out` helpers on the same buffered stdout handle.
    //
    // FR-192: this bypass predates FR-191's ordering fence and did NOT carry
    // it, which was its own miscompile -- `printf("[%s] %d\n", argv[1],
    // noisy())` with a stdout-writing `noisy()` printed `[hello<1>] 1` where
    // the native prints `<1>[hello] 1`, because the raw write went out before
    // the argument that produced `<1>` had even been evaluated. It now takes
    // the same fence as the two bypasses below, and the declined call falls
    // through to the deferred `%s` path.
    if (allowRawBypass && mainArgvTableValue && !laterArgSideEffects) {
      if (spec == 's') {
        if (const clang::Expr *idxExpr = matchArgvWholeSubscript(argExpr)) {
          if (rawBypassed)
            *rawBypassed = true;
          if (failed(flushSegment()))
            return failure();
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
          if (rawBypassed)
            *rawBypassed = true;
          if (failed(flushSegment()))
            return failure();
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
      // FR-192: an argv `%s` whose bypass the fence above just declined has
      // nowhere else to go -- `emitPrintfStringArg`'s shapes do not admit an
      // argv element, which reaches a format hole only through the argv
      // table. Rather than lose the shape to a rejection, it is deferred like
      // any other `%s` and rendered through the Latin-1 `__emitrust_cstr`
      // funnel at materialization time. Only the raw-byte property is given
      // up, exactly the trade FR-191 already recorded for char regions; the
      // ordering, which is what was actually wrong, is now right.
      if (allowRawBypass && mainArgvTableValue && laterArgSideEffects &&
          !argExpr->HasSideEffects(astContext()))
        if (const clang::Expr *idxExpr = matchArgvWholeSubscript(argExpr)) {
          deferredStrings.push_back(DeferredString{operands.size(),
                                                   /*argExpr=*/nullptr,
                                                   stringPrecision, idxExpr});
          operands.push_back(Value());
          rustFormat += textPlaceholder();
          continue;
        }
      // FR-191: in a stdout `print!` context a `%s` over a char REGION
      // prints its raw bytes, bypassing the `__emitrust_cstr` Latin-1
      // Display funnel whose per-byte `u8 as char` widening re-encodes
      // every byte >= 0x80 as TWO UTF-8 bytes (a measured miscompile:
      // native `ff fe 81 7a` vs emitted `c3 bf c3 be c2 81 7a`). One
      // restriction keeps it byte-exact: flushing the pending segment
      // moves this call's output BEFORE the evaluation of the arguments
      // still to come, where C evaluates every argument before printf
      // writes anything. A later argument with side effects (it could
      // write the very buffer being printed) therefore declines the
      // bypass.
      // FR-193 item 2 REMOVED the second restriction FR-191 recorded here,
      // "a FIELD WIDTH pads to a byte count only the formatter knows, and
      // the raw write cannot pad". The premise was true of `write_all`
      // alone and the conclusion was wrong: C pads to the BYTE length of
      // the converted run, which is precisely the length the helper's NUL
      // scan already computes, so `__emitrust_cstr_pad_out` writes padding
      // and payload as one raw run. Leaving the shape on the funnel was a
      // live miscompile, not a scope note — `printf("%10s")` over `81 8e
      // 9b a8 7a` emitted five correct pad bytes followed by NINE payload
      // bytes where C writes five.
      // The argument is materialized either way — the ops are identical,
      // only the wrapping differs — so nothing is evaluated twice and the
      // C argument order is preserved.
      bool wantRawBytes = allowRawBypass && !laterArgSideEffects;
      // FR-192: with a side-effecting argument still to come, the region has
      // to be read AFTER that argument runs, so the whole `%s` lowering moves
      // to `materializeDeferredStrings` and only its operand slot is reserved
      // here. Two exclusions keep the reordering to the cases that can
      // actually observe a write:
      //   * a STRING LITERAL argument (`__func__` included) is immutable in C
      //     -- writing through it is undefined -- so nothing a later argument
      //     does can change what the conversion reads. Moving it would be
      //     pure churn in the emitted bytes for no semantic gain;
      //   * an argument expression that itself has SIDE EFFECTS is left
      //     alone: C leaves the relative order of two side-effecting
      //     arguments unspecified, and reordering one would be a gratuitous
      //     divergence from the native binary the byte-diff oracle compares
      //     against.
      if (laterArgSideEffects &&
          !underlyingStringLiteral(argExpr->IgnoreParenImpCasts()) &&
          !argExpr->HasSideEffects(astContext())) {
        deferredStrings.push_back(DeferredString{operands.size(), argExpr,
                                                 stringPrecision,
                                                 /*argvIndexExpr=*/nullptr});
        operands.push_back(Value());
        rustFormat += textPlaceholder();
        continue;
      }
      bool rawByteSlice = false;
      FailureOr<Value> text = emitPrintfStringArg(
          argExpr, stringPrecision, wantRawBytes ? &rawByteSlice : nullptr);
      if (failed(text))
        return failure();
      if (rawByteSlice) {
        if (rawBypassed)
          *rawBypassed = true;
        if (failed(flushSegment()))
          return failure();
        if (!width.empty()) {
          // FR-193 item 2: a FIELD WIDTH pads to the C BYTE length of the
          // converted run. The width, the precision and the '-' flag are
          // all compile-time constants here ('*' width/precision and the
          // '0' flag are located rejections above), so they travel as i32
          // constants exactly like the `__emitrust_fmt_*` helpers'; only
          // the run length is a runtime quantity, and the helper's own NUL
          // scan already has it. `prec` < 0 spells "no precision".
          needsCStrPadOutHelper = true;
          Value precValue = createIntConstant(
              loc, i32Type,
              stringPrecision ? static_cast<int64_t>(*stringPrecision) : -1);
          Value widthConst = createIntConstant(loc, i32Type, widthValue);
          Value flagsValue = createIntConstant(loc, i32Type, flagsMask);
          builder.create<emitrust::CallOpaqueOp>(
              loc, TypeRange(),
              builder.getStringAttr("__emitrust_cstr_pad_out"),
              /*args=*/ArrayAttr(),
              ValueRange{*text, precValue, widthConst, flagsValue});
        } else if (stringPrecision) {
          // `%.Ns`: at most N raw bytes, stopping earlier at a NUL.
          needsCStrNOutHelper = true;
          Value count =
              createIntConstant(loc, builder.getIntegerType(64),
                                static_cast<int64_t>(*stringPrecision));
          builder.create<emitrust::CallOpaqueOp>(
              loc, TypeRange(),
              builder.getStringAttr("__emitrust_cstr_n_out"),
              /*args=*/ArrayAttr(), ValueRange{*text, count});
        } else {
          needsCStrOutHelper = true;
          builder.create<emitrust::CallOpaqueOp>(
              loc, TypeRange(), builder.getStringAttr("__emitrust_cstr_out"),
              /*args=*/ArrayAttr(), ValueRange{*text});
        }
        continue;
      }
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
      // C converts the argument to unsigned char and writes THAT ONE
      // CHARACTER (C11 7.21.6.1p8) — exactly one byte for every value
      // 0..255.
      if (!isIntArgument)
        return emitError(loc) << "unsupported: printf argument " << argNumber
                              << " does not match its format specifier";
      // FR-194: in a stdout `print!` context the byte goes out RAW, through
      // the same `__emitrust_byte_out` helper the argv `%c` path and the
      // `std::ostream << char` operand already use. The `__emitrust_fmt_c`
      // Display funnel below is a Latin-1 widening to a Unicode scalar and
      // `Display for char` writes UTF-8, so every byte >= 0x80 came out as
      // TWO bytes (measured on a 256-value sweep: 256 native stdout bytes
      // vs 384 emitted, first differing at offset 0x80).
      //
      // A FIELD WIDTH is no obstacle here, unlike `%s`: C pads the ONE byte
      // to `width` columns with spaces, and `width` is a compile-time
      // constant, so the padding is literal text in the format segments
      // around the raw write and needs no formatter. What DOES decline the
      // bypass is the ordering fence FR-191 established: flushing the
      // pending segment (and writing this byte) moves output BEFORE the
      // evaluation of the arguments still to come, where C evaluates every
      // argument before printf writes anything. A later argument with side
      // effects therefore keeps the whole call on the Display funnel — the
      // same restriction, reusing the same mechanism, rather than a second
      // one.
      bool wantRawByte = allowRawBypass && !laterArgSideEffects;
      if (wantRawByte) {
        if (rawBypassed)
          *rawBypassed = true;
        std::string pad(widthValue > 1 ? widthValue - 1 : 0, ' ');
        if (!leftAlign)
          rustFormat += pad;
        if (failed(flushSegment()))
          return failure();
        Value byte = castToIntType(loc, *argument, builder.getIntegerType(8));
        needsByteOutHelper = true;
        builder.create<emitrust::CallOpaqueOp>(
            loc, TypeRange(), builder.getStringAttr("__emitrust_byte_out"),
            /*args=*/ArrayAttr(), ValueRange{byte});
        if (leftAlign)
          rustFormat += pad;
        continue;
      }
      // A buffer context (`sprintf`/`snprintf`) and the declined-bypass
      // stdout case keep the Latin-1 funnel: the i32 argument (chars arrive
      // int-promoted) becomes the `char` whose code point IS the byte. In
      // the buffer context that is exact, because `__emitrust_sprintf`
      // decodes the formatted `String` back one byte per `char`.
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
  // FR-192: every argument has now been lowered, so the deferred `%s` reads
  // happen here -- after the last side effect, before the caller's
  // `print!`/`format!` consumes `operands`.
  if (failed(materializeDeferredStrings()))
    return failure();
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
                               std::optional<unsigned> precision,
                               bool *rawByteSlice) {
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
    // FR-191: a caller that can consume RAW BYTES (a stdout `print!`
    // position) takes the `&[i8]` unwrapped — the Latin-1 `u8 as char`
    // widening inside these helpers re-encodes every byte >= 0x80 as two
    // UTF-8 bytes, which is a miscompile against C's single byte. The
    // element-type test is what makes the handoff sound: the raw `*_out`
    // helpers are pinned to `&[i8]`, so any other element type keeps the
    // Display funnel rather than being mistyped at the call.
    if (rawByteSlice)
      if (auto refType = llvm::dyn_cast<emitrust::RefType>(slice.getType()))
        if (auto sliceType =
                llvm::dyn_cast<emitrust::SliceType>(refType.getPointee()))
          if (sliceType.getElementType() == builder.getIntegerType(8)) {
            *rawByteSlice = true;
            return slice;
          }
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

FailureOr<Value> CImporter::wrapStringPushChar(Location loc,
                                               const clang::Expr *argExpr,
                                               Value value,
                                               llvm::StringRef entity) {
  // FR-194: this is a STORE, not a rendering. A C++ `std::string` is a byte
  // sequence that may hold any byte; a Rust `String` is UTF-8 by invariant
  // and cannot hold a lone byte >= 0x80 at all, so pushing the Latin-1
  // `char` for such a byte stores its two-byte UTF-8 encoding and `size()`
  // then reports 2 where C++ reports 1 (measured on `s += (char)0xc8`).
  // There is no encoding that fixes that inside the current model, so the
  // only outcomes available are rejection and a loud failure.
  //
  // A constant operand is decidable right here and gets the located
  // rejection this repo prefers. A runtime operand is not, and rejecting
  // every one of them would refuse the whole ASCII world of
  // character-at-a-time string building for the sake of a value that may
  // never occur, so it takes the runtime guard instead: silence is what is
  // forbidden, not lateness.
  clang::Expr::EvalResult constant;
  if (argExpr->EvaluateAsInt(constant, astContext())) {
    uint64_t byte = constant.Val.getInt().getExtValue() & 0xff;
    if (byte >= 0x80)
      return emitError(loc)
             << "unsupported: " << entity
             << " of a byte >= 0x80: a Rust String is UTF-8 and cannot "
                "hold it";
    // A constant that IS ASCII needs no guard at all.
    return wrapCharFormat(loc, value);
  }
  needsAsciiCharHelper = true;
  Value promoted = castToIntType(loc, value, builder.getI32Type());
  auto charType = emitrust::OpaqueType::get(builder.getContext(), "char");
  return builder
      .create<emitrust::CallOpaqueOp>(
          loc, TypeRange{charType},
          builder.getStringAttr("__emitrust_ascii_char"),
          /*args=*/ArrayAttr(), ValueRange{promoted})
      .getResult(0);
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
  // value matches byte-for-byte.
  //
  // FR-191: `puts` is the SAME stdout position as a bare `printf("%s\n")`
  // and had the same defect — a char region routed through the Latin-1
  // `__emitrust_cstr` Display funnel prints two UTF-8 bytes for every byte
  // >= 0x80 where C writes one. A region argument therefore writes its raw
  // bytes and then the bare `println!()` that supplies puts' newline; there
  // is no format hole and no later argument, so no ordering question
  // arises. The shapes that never reached the funnel (a string literal, an
  // FR-64 lifted `String`) keep the single `println!("{}", s)`.
  bool rawByteSlice = false;
  FailureOr<Value> text =
      emitPrintfStringArg(call->getArg(0), std::nullopt, &rawByteSlice);
  if (failed(text))
    return failure();
  if (rawByteSlice) {
    needsCStrOutHelper = true;
    builder.create<emitrust::CallOpaqueOp>(
        loc, TypeRange(), builder.getStringAttr("__emitrust_cstr_out"),
        /*args=*/ArrayAttr(), ValueRange{*text});
    builder.create<emitrust::CallOpaqueOp>(
        loc, TypeRange(), builder.getStringAttr("println!"),
        builder.getArrayAttr({}), ValueRange());
    return success();
  }
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

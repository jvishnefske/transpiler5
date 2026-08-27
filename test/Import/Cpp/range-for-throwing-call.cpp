// FR-61f-d, defect leg: a call to a can-throw-closure function inside a
// range-eligible `for` body must REFUSE the lift.
//
// This pins a PRE-EXISTING CRASH, reachable at HEAD with no `if` anywhere.
// `unwrapThrowsResult` (ImportCStatements.cpp) lowers a throwing call to
// `createBlock()` x2 + `cf::CondBranchOp`, and `createBlock` appends to the
// FUNCTION region. With the insertion point inside the single-block
// `emitrust.for` region, the `cf.cond_br` names successors that are not in the
// same region, and the MLIR verifier dereferences a null successor:
// `mlir::OpTrait::impl::verifyOneSuccessor` faults with "PLEASE submit a bug
// report to https://github.com/llvm/llvm-project/issues/". `blocksRangeForLift`
// had no case for a CallExpr whose callee is in `throwsClosure`.
//
// All three C++ exception shapes crash at HEAD and are fenced
// together here: `clang::CXXThrowExpr` and `clang::CXXTryStmt` join the isa
// list, and a CallExpr whose direct callee is in `throwsClosure` is refused
// explicitly. (The `throw`-inside-an-`if` leg is reachable today only because
// the `if` itself blocks first; the FR-61f-d widening removes that shield, so
// the fence has to be in place before it lands.) Reading the closure keeps `matchRangeFor` pure: it is a
// module-level planning result, stable across the pre-pass and emission, the
// same way `addressTaken` is.
//
// The lesson worth pinning, and the reason this file exists: `blocksRangeForLift`
// is a BLOCKLIST, and blocklists leak. Every fence entry here is a
// block-creating emitter, and this one was missed because it is spelled as an
// ordinary call.
//
// RUN: emitrust-import-c %s | FileCheck %s

// CHECK-LABEL: func.func @thrower
static int thrower(int x) {
  if (x > 100)
    throw 5;
  return x + 1;
}

// A THROWING CALL in the body: no `if` in this loop at all, yet the lift must
// be refused -- the call itself creates the cf edge. Crashed the verifier
// before the fence gained the throws pair.
// CHECK-LABEL: func.func @run_calls
// CHECK-NOT:     emitrust.for
// CHECK:         cf.
static int run_calls(int n) {
  int ok = 0;
  try {
    for (int i = 0; i < n; ++i)
      ok += thrower(i);
  } catch (int e) {
    ok = -e;
  }
  return ok;
}

// A `throw` written DIRECTLY inside an `if` in the body. Today the `if` blocks
// this loop on its own; `CXXThrowExpr` is what keeps it blocked once the
// FR-61f-d widening admits `if` into the region.
// CHECK-LABEL: func.func @run_throws
// CHECK-NOT:     emitrust.for
// CHECK:         cf.
static int run_throws(int n) {
  int ok = 0;
  try {
    for (int i = 0; i < n; ++i) {
      if (i > 3)
        throw 7;
      ok += i;
    }
  } catch (int e) {
    ok = -e;
  }
  return ok;
}

// A `try` block nested in the body: `CXXTryStmt` fences it for the same reason.
// CHECK-LABEL: func.func @try_in_body
// CHECK-NOT:     emitrust.for
// CHECK:         cf.
static int try_in_body(int n) {
  int ok = 0;
  for (int i = 0; i < n; ++i) {
    try {
      ok += thrower(i);
    } catch (int e) {
      ok = -e;
    }
  }
  return ok;
}

int use(int n) { return run_calls(n) + run_throws(n) + try_in_body(n); }

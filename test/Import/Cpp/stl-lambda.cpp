// RUN: emitrust-import-c %s | FileCheck %s
// W2.13: by-value-capture lambda via LAMBDA LIFTING (task-010; flips
// Cpp17Suite 00903). A local `auto f = [a, b](int x) {...};` lifts to a
// module-level synthesized fn (`<function>_<name>`, the block-scope
// mangle convention) with the captures PREPENDED as parameters, FROZEN
// at the declaration point (one load per capture — this IS C++'s
// capture-by-value semantics: a mutation of the source variable between
// the lambda's creation and a call must not be visible to that call),
// and every direct `f(args)` call rewritten to
// `lifted(frozen..., args...)`. Pins: the lifted fn's signature with the
// prepended capture parameter and its `emitrust.param_names` (capture
// names first); the freeze-load at the DECLARATION point ordered BEFORE
// a later store to the same variable, with BOTH call sites passing the
// same frozen value; a TWO-capture lambda pinning prepended-capture
// order; a zero-argument lambda call; a zero-capture lambda (the lift's
// degenerate case: an empty frozen list).

// The freeze-load happens at the lambda's declaration, BEFORE the
// `base = 99` store; both calls pass that same frozen value, so the
// later mutation is invisible to them (capture-by-value).
// CHECK-LABEL: func.func @single
// CHECK: %[[BASE:.*]] = memref.alloca() : memref<i32>
// CHECK: %[[FROZEN:.*]] = memref.load %[[BASE]][]
// CHECK: memref.store %{{.*}}, %[[BASE]][]
// CHECK: call @single_add(%[[FROZEN]], %{{.*}}) : (i32, i32) -> i32
// CHECK: call @single_add(%[[FROZEN]], %{{.*}}) : (i32, i32) -> i32
int single(void) {
  int base = 30;
  auto add = [base](int x) { return base + x; };
  base = 99; // Frozen above: both calls must still add 30.
  return add(4) + add(12);
}

// The lifted fn: module-level, capture parameter PREPENDED, both
// parameters named through emitrust.param_names (capture name first),
// body reading the capture through its own parameter binding.
// CHECK-LABEL: func.func @single_add
// CHECK-SAME: (%{{.*}}: i32, %{{.*}}: i32) -> i32
// CHECK-SAME: emitrust.param_names = ["base", "x"]
// CHECK: arith.addi
// CHECK: return

// Two captures: frozen in capture-list order, prepended in that order.
// CHECK-LABEL: func.func @two_caps
// Entry allocas hoist in reverse declaration order, so the cells are
// identified by their stored constants, not by alloca position: a=3 first,
// b=40 second, then the freeze-loads in capture-list order feed the call.
// CHECK: %[[CA:.*]] = arith.constant 3
// CHECK: memref.store %[[CA]], %[[A:.*]][]
// CHECK: %[[CB:.*]] = arith.constant 40
// CHECK: memref.store %[[CB]], %[[B:.*]][]
// CHECK: %[[FA:.*]] = memref.load %[[A]][]
// CHECK: %[[FB:.*]] = memref.load %[[B]][]
// CHECK: call @two_caps_mix(%[[FA]], %[[FB]], %{{.*}}) : (i32, i32, i32) -> i32
int two_caps(void) {
  int a = 3;
  int b = 40;
  auto mix = [a, b](int x) { return a * 100 + b * 10 + x; };
  return mix(5);
}

// CHECK-LABEL: func.func @two_caps_mix
// CHECK-SAME: (%{{.*}}: i32, %{{.*}}: i32, %{{.*}}: i32) -> i32
// CHECK-SAME: emitrust.param_names = ["a", "b", "x"]

// A zero-argument lambda: the call passes only the frozen capture.
// CHECK-LABEL: func.func @no_args
// CHECK: %[[N:.*]] = memref.alloca() : memref<i32>
// CHECK: %[[FN:.*]] = memref.load %[[N]][]
// CHECK: call @no_args_get(%[[FN]]) : (i32) -> i32
// CHECK: call @no_args_get(%[[FN]]) : (i32) -> i32
int no_args(void) {
  int n = 7;
  auto get = [n]() { return n; };
  return get() + get();
}

// CHECK-LABEL: func.func @no_args_get
// CHECK-SAME: (%{{.*}}: i32) -> i32
// CHECK-SAME: emitrust.param_names = ["n"]

// Zero captures: the frozen list is empty and the lift degenerates to an
// ordinary module-level function of the lambda's own parameters.
// CHECK-LABEL: func.func @zero_capture
// CHECK: call @zero_capture_inc(%{{.*}}) : (i32) -> i32
int zero_capture(void) {
  auto inc = [](int x) { return x + 1; };
  return inc(41);
}

// CHECK-LABEL: func.func @zero_capture_inc
// CHECK-SAME: (%{{.*}}: i32) -> i32
// CHECK-SAME: emitrust.param_names = ["x"]

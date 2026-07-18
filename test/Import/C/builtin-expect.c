// RUN: emitrust-import-c %s | FileCheck %s

// `__builtin_expect(e, c)` is a pure branch-prediction hint: it folds to
// its first argument during import. No call op, opaque remnant, or
// `__builtin_expect` symbol may survive into the IR, and a folded constant
// condition composes with constant-condition dead-arm elision (the 00214
// `if (__builtin_expect(!!(0), 0))` shape).

// CHECK-NOT: __builtin_expect

int expect_cond(int x) {
  if (__builtin_expect(x, 1))
    return 2;
  return 3;
}

// The condition lowers exactly like a plain `if (x)`: a truth test and a
// conditional branch, no call.
// CHECK-LABEL: func.func @expect_cond
// CHECK-NOT: call
// CHECK: arith.cmpi ne
// CHECK: cf.cond_br
// CHECK: return

long expect_value(long a, long b) {
  long v = __builtin_expect(a + b, 0);
  return v;
}

// In value position the fold leaves just the argument expression.
// CHECK-LABEL: func.func @expect_value
// CHECK-NOT: call
// CHECK: arith.addi %{{.*}} : i64
// CHECK: return

int expect_bang_bang(int g) {
  if (__builtin_expect(!!(g == 5), 0))
    return 7;
  return 8;
}

// The kernel-style `!!(...)` wrapper folds through: the comparison lowers
// and the double-negation is ordinary boolean plumbing.
// CHECK-LABEL: func.func @expect_bang_bang
// CHECK-NOT: call
// CHECK: arith.cmpi eq
// CHECK: return

int expect_const_dead(int r) {
  if (__builtin_expect(!!(0), 0)) {
    r = r * 31;
  }
  return r;
}

// After the fold the condition is the compile-time constant `!!(0)`; the
// dead arm (no labels) is elided before lowering, so its body constant
// never appears.
// CHECK-LABEL: func.func @expect_const_dead
// CHECK-NOT: arith.constant 31
// CHECK: return

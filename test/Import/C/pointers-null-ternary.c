// RUN: emitrust-import-c %s | FileCheck %s

// CTS-P9 (the 00144 shapes): a ConditionalOperator is a pointer source.
// Classifying a pointer-typed ternary classifies both arms and unites
// their regions; a null-pointer-constant arm marks the united region
// nullable, reusing the existing non-null flag-cell machinery (no new
// representation). A region that never sees an address — only null
// constants and other null-only pointers — is base-less and therefore
// statically null: it carries no runtime state at all, its null tests
// fold to constants, and a pointer-to-int cast of it folds to 0.
// Dereference of a null-only pointer stays rejected (see
// pointers-null-ternary-invalid.c).

// The 00144 int arms: a plain integer conditional with a long arm
// truncated back into an int.
int int_arm_widen(int i) {
  i = i ? 0 : 0l;
  return i;
}
// CHECK-LABEL: func.func @int_arm_widen
// CHECK: return

// The 00144 pointer chain: every arm is a null constant (in both arm
// orders, with `void *` and qualification variants) or a pointer that is
// itself null-only. The united region is base-less, so no flag cell or
// cursor cell exists, `if (q)` folds to a constant-false branch, and
// `(int) q` folds to the constant 0.
int null_only_ternaries(int i) {
  int *q;
  void *p;
  p = i ? (void *) 0 : 0;
  p = i ? 0 : (void *) 0;
  p = i ? 0 : (const void *) 0;
  q = i ? 0 : p;
  q = i ? p : 0;
  q = i ? q : 0;
  q = i ? 0 : q;
  if (q)
    return 1;
  return (int) q;
}
// CHECK-LABEL: func.func @null_only_ternaries
// CHECK-NOT: memref.alloca
// CHECK: %[[F:.*]] = arith.constant false
// CHECK: cf.cond_br %[[F]]
// CHECK: %[[Z:.*]] = arith.constant 0 : i32
// CHECK: return %[[Z]] : i32

// An equality null test of a statically-null pointer folds to true.
int null_test_eq(int i) {
  int *q;
  q = i ? 0 : 0;
  if (q == 0)
    return 7;
  return 8;
}
// CHECK-LABEL: func.func @null_test_eq
// CHECK-NOT: memref.alloca
// CHECK: %[[T:.*]] = arith.constant true
// CHECK: cf.cond_br %[[T]]

// A ternary with a real address arm: `q = i ? &x : 0` is the ordinary
// nullable region. The ternary branches on its condition, the address
// arm stores true into the flag cell and the null arm stores false; the
// null test reads the flag, and the deref is guarded.
int ternary_real_base(int i) {
  int x = 7;
  int *q;
  q = i ? &x : 0;
  if (q)
    return *q;
  return -1;
}
// CHECK-LABEL: func.func @ternary_real_base
// CHECK: %[[FLAG:.*]] = memref.alloca() : memref<i1>
// CHECK: %[[X:.*]] = emitrust.variable named "x" : !emitrust.lvalue<i32>
// CHECK: cf.cond_br
// CHECK: %[[TRUE:.*]] = arith.constant true
// CHECK: memref.store %[[TRUE]], %[[FLAG]][] : memref<i1>
// CHECK: %[[FALSE:.*]] = arith.constant false
// CHECK: memref.store %[[FALSE]], %[[FLAG]][] : memref<i1>
// CHECK: %[[NN:.*]] = memref.load %[[FLAG]][] : memref<i1>
// CHECK: cf.cond_br %[[NN]]
// CHECK: emitrust.call_opaque "assert!"(%{{.*}}) {args = [0 : index, "null pointer dereference"]}
// CHECK: emitrust.load %[[X]]

// The swapped arm order: the null arm (then) stores false, the address
// arm (else) stores true.
int ternary_real_base_swapped(int i) {
  int y = 9;
  int *q;
  q = i ? 0 : &y;
  if (q != 0)
    return *q;
  return -1;
}
// CHECK-LABEL: func.func @ternary_real_base_swapped
// CHECK: %[[FLAG2:.*]] = memref.alloca() : memref<i1>
// CHECK: cf.cond_br
// CHECK: %[[FALSE2:.*]] = arith.constant false
// CHECK: memref.store %[[FALSE2]], %[[FLAG2]][] : memref<i1>
// CHECK: %[[TRUE2:.*]] = arith.constant true
// CHECK: memref.store %[[TRUE2]], %[[FLAG2]][] : memref<i1>
// CHECK: %[[NN2:.*]] = memref.load %[[FLAG2]][] : memref<i1>
// CHECK: emitrust.call_opaque "assert!"(%{{.*}}) {args = [0 : index, "null pointer dereference"]}

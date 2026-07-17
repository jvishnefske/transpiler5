// RUN: emitrust-import-c %s | FileCheck %s

// CTS-P8: NULL data-pointer constants. A pointer region that sees a null
// pointer constant is nullable: each of its pointers models an Option of
// its cursor, mirroring the fn_ptr None mapping. The discriminant lives in
// a promotable rank-0 memref<i1> "non-null" flag cell: NULL assignment
// stores false, an address binding stores true, a null-check (`if (p)`,
// `p == 0`, `p != NULL`) reads the flag, and a dereference of a
// possibly-null pointer is guarded by a deterministic panic
// (`assert!(flag, "null pointer dereference")` — C dereferencing null is
// UB, so the panic is a legal refinement). Pointers whose region never
// sees NULL fold their null-checks to constants.

// NULL init, a truth-value null-check branch, an equality null-check,
// re-pointing at an object, and a guarded dereference (the 00171 shapes
// plus the re-point-then-deref idiom).
int null_init_repoint(void) {
  int x = 5;
  int *p = 0;
  if (p)
    return 1;
  p = &x;
  if (p == 0)
    return 2;
  return *p;
}
// CHECK-LABEL: func.func @null_init_repoint
// CHECK: %[[FLAG:.*]] = memref.alloca() : memref<i1>
// CHECK: %[[FALSE:.*]] = arith.constant false
// CHECK: memref.store %[[FALSE]], %[[FLAG]][] : memref<i1>
// CHECK: %[[T0:.*]] = memref.load %[[FLAG]][] : memref<i1>
// CHECK: cf.cond_br %[[T0]]
// CHECK: %[[TRUE:.*]] = arith.constant true
// CHECK: memref.store %[[TRUE]], %[[FLAG]][] : memref<i1>
// CHECK: %[[T1:.*]] = memref.load %[[FLAG]][] : memref<i1>
// CHECK: %[[NOT:.*]] = arith.xori %[[T1]], %{{.*}} : i1
// CHECK: cf.cond_br %[[NOT]]
// CHECK: %[[T2:.*]] = memref.load %[[FLAG]][] : memref<i1>
// CHECK: emitrust.call_opaque "assert!"(%[[T2]]) {args = [0 : index, "null pointer dereference"]} : (i1) -> ()
// CHECK: emitrust.load

// A pointer that only ever holds the null constant has no base object; it
// still carries its discriminant, so null-checks work (dereference is
// rejected, see pointers-null-invalid.c).
int null_only(void) {
  int *c;
  c = 0;
  if (c != 0)
    return 1;
  return 0;
}
// CHECK-LABEL: func.func @null_only
// CHECK: %[[CFLAG:.*]] = memref.alloca() : memref<i1>
// CHECK: %[[CF:.*]] = arith.constant false
// CHECK: memref.store %[[CF]], %[[CFLAG]][] : memref<i1>
// CHECK: %[[CV:.*]] = memref.load %[[CFLAG]][] : memref<i1>
// CHECK: cf.cond_br %[[CV]]

// A pointer whose region never sees NULL is statically non-null: it needs
// no flag cell, and its null-check folds to a constant.
int static_not_null(void) {
  int a = 42;
  int *b = &a;
  if (b == 0)
    return 1;
  return *b;
}
// CHECK-LABEL: func.func @static_not_null
// CHECK-NOT: memref.alloca() : memref<i1>
// CHECK: %[[BF:.*]] = arith.constant false
// CHECK: cf.cond_br %[[BF]]
// CHECK-NOT: emitrust.call_opaque "assert!"
// CHECK: return

// Copying a pointer (`q = p`) copies its discriminant: both pointers share
// one nullable region, and each carries its own flag cell.
int copy_flag(void) {
  int x = 3;
  int *p = 0;
  int *q;
  p = &x;
  q = p;
  if (q)
    return *q;
  return 0;
}
// CHECK-LABEL: func.func @copy_flag
// CHECK: %[[QFLAG:.*]] = memref.alloca() : memref<i1>
// CHECK: %[[PFLAG:.*]] = memref.alloca() : memref<i1>
// CHECK: memref.store %{{.*}}, %[[PFLAG]][] : memref<i1>
// CHECK: memref.store %{{.*}}, %[[PFLAG]][] : memref<i1>
// CHECK: %[[PV:.*]] = memref.load %[[PFLAG]][] : memref<i1>
// CHECK: memref.store %[[PV]], %[[QFLAG]][] : memref<i1>
// CHECK: %[[QV:.*]] = memref.load %[[QFLAG]][] : memref<i1>
// CHECK: cf.cond_br %[[QV]]
// CHECK: emitrust.call_opaque "assert!"

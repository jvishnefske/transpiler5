// RUN: emitrust-import-c %s | FileCheck %s

// C99-7 type qualifiers, accepted side. const maps positionally: a
// never-written const global (scalar or array) is a `const`-marked
// emitrust.global (an immutable Rust `static`), a `static const` local
// becomes the same mangled module global, a const-qualified local keeps
// the ordinary variable/alloca lowering (writes are already a clang
// frontend error, and mem2reg renders the SSA form as immutable lets),
// and a const pointee (`const int *p`) classifies exactly like its
// unqualified spelling — the parameter shape (mut_ref or slice) comes
// from the region analysis, not the qualifier. restrict is an aliasing
// hint the region analysis is already stricter than: it is accepted and
// ignored, so a restrict-qualified parameter imports identically to the
// unqualified one. Qualification-only pointer casts (adding or dropping
// const) are transparent (CTS-P2 peeling). volatile and _Atomic are
// rejected by policy (see qualifiers-invalid.c) with one exception:
// qualifiers on a parameter OBJECT itself (`volatile int v`,
// `int x[volatile 5]` which adjusts to `int * volatile x`) are
// body-local, never part of the function type, and accepted-and-ignored
// (c-testsuite 00162).

// Const globals: scalar and array both carry the `const` marker.
const int SCALE = 3;
const short STEPS[4] = {1, 2, 3, 4};
// CHECK-DAG: emitrust.global const @SCALE <3 : i32> : i32
// CHECK-DAG: emitrust.global const @STEPS <[1 : i16, 2 : i16, 3 : i16, 4 : i16]> : !emitrust.array<4xi16>

// A const pointee classifies like `int *`: deref-only stays a scalar
// mut_ref reference.
int read_const(const int *p) { return *p; }
// CHECK-LABEL: func.func @read_const
// CHECK-SAME: (%[[P:.*]]: !emitrust.mut_ref<i32>)
// CHECK: emitrust.deref %[[P]] : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>

// A const value parameter is an ordinary by-value scalar.
int scale_by(const int factor) { return factor * 2; }
// CHECK-LABEL: func.func @scale_by
// CHECK-SAME: (%{{.*}}: i32) -> i32

// restrict on a deref-only parameter: identical to the unqualified
// mut_ref shape.
int deref_restrict(int *restrict p) { return *p; }
// CHECK-LABEL: func.func @deref_restrict
// CHECK-SAME: (%[[R:.*]]: !emitrust.mut_ref<i32>)
// CHECK: emitrust.deref %[[R]] : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>

// const + restrict on a subscripted parameter: the region analysis
// still drives the shape (here the owner-promoted array of main).
int sum_restrict(const int *restrict a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s = s + a[i];
  return s;
}
// CHECK-LABEL: func.func @sum_restrict
// CHECK: emitrust.subscript

// A qualifier inside a parameter's array bound applies to the adjusted
// pointer object itself (`int * volatile x`), which is body-local:
// accepted and ignored, the parameter classifies exactly like
// `int x[5]` (here: owner-promoted with its lone array argument).
void qual_array(int x[volatile 5]) { x[3] = 42; }
// CHECK-LABEL: func.func @qual_array
// CHECK-SAME: (%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"Owner_main_q">>, %{{.*}}: i64)

// A top-level volatile on a by-value parameter is likewise a body-local
// copy: accepted and ignored.
int vol_value(volatile int v) { return v + 1; }
// CHECK-LABEL: func.func @vol_value
// CHECK-SAME: (%{{.*}}: i32) -> i32

// Qualification-only casts peel (CTS-P2): `(const int *)&x` and the
// cast back to `int *` are both transparent, so the write through `mp`
// and the read through `cp` resolve to the same variable place.
int cast_qualifiers(void) {
  int x = 9;
  const int *cp = (const int *)&x;
  int *mp = (int *)cp;
  *mp = 11;
  return *cp;
}
// CHECK-LABEL: func.func @cast_qualifiers
// CHECK: %[[X:.*]] = emitrust.variable : !emitrust.lvalue<i32>
// CHECK: %[[C9:.*]] = arith.constant 9 : i32
// CHECK: emitrust.assign %[[X]] = %[[C9]] : !emitrust.lvalue<i32>
// CHECK: %[[C11:.*]] = arith.constant 11 : i32
// CHECK: emitrust.assign %[[X]] = %[[C11]] : !emitrust.lvalue<i32>
// CHECK: emitrust.load %[[X]] : (!emitrust.lvalue<i32>) -> i32

// A `const char *` bound to a string literal keeps the const-marked
// backing array of the literal region (CTS-P1).
int string_arg(void) {
  const char *s = "hi";
  int n = 0;
  while (*s) { n++; s++; }
  return n;
}
// CHECK-LABEL: func.func @string_arg
// CHECK: emitrust.variable const <[104 : i8, 105 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<3xi8>>

int main(void) {
  // A const local scalar keeps the ordinary variable lowering; its
  // address feeds the const-pointee parameter through the usual mut
  // borrow (the emitted binding is mutable, which is sound: clang has
  // already rejected any write through a const lvalue).
  const int local = 5;
  // CHECK-LABEL: func.func @c_main
  // CHECK: %[[L:.*]] = emitrust.variable : !emitrust.lvalue<i32>
  // CHECK: emitrust.assign %[[L]] = %{{.*}} : !emitrust.lvalue<i32>
  // A const local array takes the same place + per-element-assign
  // lowering as a mutable one.
  const int arr[2] = {10, 20};
  // CHECK: %[[ARR:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<2xi32>>
  // CHECK: emitrust.subscript %[[ARR]][%{{.*}}]
  // A function-local `static const` is a mangled const module global.
  static const int cached = 40;
  int r = read_const(&local);
  // CHECK: %[[LREF:.*]] = emitrust.addr_of mut %[[L]] : (!emitrust.lvalue<i32>) -> !emitrust.mut_ref<i32>
  // CHECK: call @read_const(%[[LREF]])
  r = r + arr[0] + arr[1];
  // Reading a const global stages through global_load like any global.
  r = r + scale_by(SCALE);
  // CHECK: emitrust.global_load @SCALE : i32
  // CHECK: call @scale_by(
  int y = 6;
  r = r + deref_restrict(&y);
  // CHECK: call @deref_restrict(
  int buf[3] = {7, 8, 9};
  r = r + sum_restrict(buf, 3);
  // CHECK: call @sum_restrict(
  int q[5] = {0, 0, 0, 0, 0};
  qual_array(q);
  // CHECK: call @qual_array(
  r = r + q[3] + vol_value(r);
  // CHECK: call @vol_value(
  r = r + cast_qualifiers();
  r = r + string_arg();
  r = r + SCALE + cached;
  // CHECK: emitrust.global_load @c_main_cached : i32
  int t = 0;
  for (int i = 0; i < 4; i++) t = t + STEPS[i];
  // CHECK: emitrust.global_load @STEPS : !emitrust.array<4xi16>
  return r + t;
}
// CHECK: emitrust.global const @c_main_cached <40 : i32> : i32

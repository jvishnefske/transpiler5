// RUN: emitrust-import-c %s | FileCheck %s

// CTS-P9: `void *` as a pointee-wildcard cursor. A `void *` carries no
// element unit of its own; bitcasts to and from `void *` are peeled in
// both the pointer-region classification and the deref emission (the
// generalization of the qualification-cast peel). A reinterpret-back site
// `*(T *)p` type-checks T against the region's base element type: an
// exact match lowers exactly like a direct pointer (the T* -> void* -> T*
// round-trip is invisible), and a same-width int<->int mismatch wraps the
// access in an `emitrust.cast` bitcast. Everything else stays rejected
// (see pointers-void-invalid.c). The `void *` itself never materializes a
// pointer value: the (base, cursor) decomposition is unchanged.

// The 00039 shape: a scalar's address parked in a `void *`, read and
// written back through `*(int *)p`. The base is a degenerate scalar, so
// the accesses resolve to x's own place with no runtime pointer state.
int roundtrip(void) {
  int x;
  void *p;
  x = 2;
  p = &x;
  *(int *)p = *(int *)p + 5;
  return *(int *)p;
}
// CHECK-LABEL: func.func @roundtrip
// CHECK-NOT: emitrust.addr_of
// CHECK: %[[X:.*]] = emitrust.variable named "x" : !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[X]]
//   *(int *)p reads and writes x's place directly.
// CHECK: %[[V:.*]] = emitrust.load %[[X]] : (!emitrust.lvalue<i32>) -> i32
// CHECK: arith.addi %[[V]]
// CHECK: emitrust.assign %[[X]]
// CHECK: emitrust.load %[[X]] : (!emitrust.lvalue<i32>) -> i32
// CHECK-NOT: emitrust.addr_of

// The 00103 shape: a `void *` holding an int*'s value, its own address
// taken into a `void **`, and the double deref cast back with `(int **)`.
// Every layer is degenerate, so `**(int **)bar` is x's place and no
// runtime state exists at any order.
int double_indirect(void) {
  int x;
  void *foo;
  void **bar;
  x = 0;
  foo = (void *)&x;
  bar = &foo;
  x = 41;
  return **(int **)bar + x;
}
// CHECK-LABEL: func.func @double_indirect
// CHECK-NOT: memref.alloca
// CHECK: %[[X2:.*]] = emitrust.variable named "x" : !emitrust.lvalue<i32>
// CHECK-NOT: emitrust.addr_of
// CHECK: emitrust.assign %[[X2]]
// CHECK: emitrust.assign %[[X2]]
// CHECK: %[[A:.*]] = emitrust.load %[[X2]] : (!emitrust.lvalue<i32>) -> i32
// CHECK: %[[B:.*]] = emitrust.load %[[X2]] : (!emitrust.lvalue<i32>) -> i32
// CHECK: arith.addi %[[A]], %[[B]]
// CHECK: return

// A `void *` as a way-station local: the int* cursor flows into v and
// back out through `(int *)v`; the region (the array plus its cursor
// cells) is untouched, and the final read subscripts the array.
int via_local(void) {
  int arr[3];
  int *q;
  void *v;
  arr[0] = 1;
  arr[1] = 2;
  arr[2] = 3;
  q = arr + 1;
  v = q;
  q = (int *)v;
  return q[1];
}
// CHECK-LABEL: func.func @via_local
// CHECK: %[[ARR:.*]] = emitrust.variable named "arr" : !emitrust.lvalue<!emitrust.array<3xi32>>
//   q = arr + 1 stores a cursor; the round-trip through v keeps it a
//   plain i64 cell copy.
// CHECK: memref.store %{{.*}}, %{{.*}}[] : memref<i64>
// CHECK: emitrust.subscript %[[ARR]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<3xi32>>, i64) -> !emitrust.lvalue<i32>
// CHECK: emitrust.load
// CHECK-NOT: emitrust.addr_of

// A same-width unsigned view over an int base: `*(unsigned int *)p`
// type-mismatches the base element at equal width, so the load wraps an
// `emitrust.cast` to the viewed type and the store casts the value back
// to the base element type before assigning.
unsigned int unsigned_view(void) {
  int x = -2;
  void *p = &x;
  unsigned int u;
  u = *(unsigned int *)p;
  *(unsigned int *)p = u + 1u;
  return u;
}
// CHECK-LABEL: func.func @unsigned_view
// CHECK: %[[X3:.*]] = emitrust.variable named "x" : !emitrust.lvalue<i32>
//   the unsigned read is x's load viewed through a bitcast
// CHECK: %[[RAW:.*]] = emitrust.load %[[X3]] : (!emitrust.lvalue<i32>) -> i32
// CHECK: emitrust.cast %[[RAW]] : i32 to ui32
//   u + 1u happens in the unsigned domain
// CHECK: emitrust.add %{{.*}}, %{{.*}} : ui32
//   the store through the unsigned view casts back to the base element
// CHECK: %[[BACK:.*]] = emitrust.cast %{{.*}} : ui32 to i32
// CHECK: emitrust.assign %[[X3]] = %[[BACK]]

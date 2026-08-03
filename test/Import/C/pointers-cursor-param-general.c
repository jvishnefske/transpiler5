// RUN: emitrust-import-c %s | FileCheck %s

// C99-43 slice 1a: cursor parameters over ANY slice element type.
//
// Pins the invariant that the CTS-00204 T** cursor-parameter machinery is
// no longer char**-only: a self-walking `int **` parameter (Shape S —
// every use under `*p`, content read-only, cursor advanced through `*p`)
// lowers to the same TWO-input form as the historical string cursor —
// a shared element slice over the region plus an in-out i64 cursor —
// with the slice element following the C element type (i32 here, i8 for
// char). The char** twin below pins that the original byte lowering is
// byte-for-byte unchanged by the generalization. The call-site protocol
// (whole-region ABSOLUTE slice from element zero, staged i64 cursor temp
// written back after the call) and the callee's entry cell round-trip
// are pinned for the i32 instance; the runtime observable is pinned
// differentially in test/EndToEnd/cursor-param-paired.c.

// Shape S over i32: `**p` read, `(*p)++` advancement.
int sum_step(int **p) {
  int v = **p;
  (*p)++;
  return v;
}
// CHECK-LABEL: func.func @sum_step
// CHECK-SAME: (%{{[^,)]+}}: !emitrust.ref<!emitrust.slice<i32>>, %{{[^,)]+}}: !emitrust.mut_ref<i64>) -> i32
// The callee copies the in-out cursor into a local cell at entry...
// CHECK: %[[BASE:.+]] = emitrust.deref %{{.+}} : (!emitrust.ref<!emitrust.slice<i32>>) -> !emitrust.lvalue<!emitrust.slice<i32>>
// CHECK: %[[CURSOR:.+]] = emitrust.deref %{{.+}} : (!emitrust.mut_ref<i64>) -> !emitrust.lvalue<i64>
// CHECK: %[[INIT:.+]] = emitrust.load %[[CURSOR]]
// CHECK: memref.store %[[INIT]]
// ...and copies it back through the reference at the return.
// CHECK: emitrust.assign %[[CURSOR]] =

// The original byte instance, unchanged by the generalization.
int take_byte(const char **s) {
  char c = **s;
  *s = *s + 1;
  return c;
}
// CHECK-LABEL: func.func @take_byte
// CHECK-SAME: (%{{[^,)]+}}: !emitrust.ref<!emitrust.slice<i8>>, %{{[^,)]+}}: !emitrust.mut_ref<i64>) -> i32

int sum3(void) {
  int arr[3] = {5, 6, 7};
  int *ip = arr;
  int t = 0;
  t = t + sum_step(&ip);
  t = t + sum_step(&ip);
  t = t + sum_step(&ip);
  return t;
}
// CHECK-LABEL: func.func @sum3
// The whole-region absolute protocol: a shared slice over the WHOLE
// array plus a staged i64 temp holding the current cursor (the slice
// starts at element zero, so callee and caller speak absolute
// coordinates; the byte-level observable is pinned differentially in
// test/EndToEnd/cursor-param-paired.c)...
// CHECK: %[[SL:.+]] = emitrust.slice_of %{{.+}} : (!emitrust.lvalue<!emitrust.array<3xi32>>, i64) -> !emitrust.ref<!emitrust.slice<i32>>
// CHECK-NEXT: %[[TMP:.+]] = emitrust.variable : !emitrust.lvalue<i64>
// CHECK-NEXT: %[[CUR:.+]] = memref.load
// CHECK-NEXT: emitrust.assign %[[TMP]] = %[[CUR]]
// CHECK-NEXT: %[[REF:.+]] = emitrust.addr_of mut %[[TMP]] : (!emitrust.lvalue<i64>) -> !emitrust.mut_ref<i64>
// CHECK-NEXT: call @sum_step(%[[SL]], %[[REF]]) : (!emitrust.ref<!emitrust.slice<i32>>, !emitrust.mut_ref<i64>) -> i32
// ...whose advanced value lands back in the caller's cursor cell.
// CHECK-NEXT: %[[ADV:.+]] = emitrust.load %[[TMP]] : (!emitrust.lvalue<i64>) -> i64
// CHECK-NEXT: memref.store %[[ADV]]

int main(void) {
  const char *q = "A";
  return sum3() + take_byte(&q) - 83;
}

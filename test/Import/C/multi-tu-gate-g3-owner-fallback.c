// W3.1 multi-TU gate oracle (G3): planOwners' owner-struct promotion
// (FR-30) requires every unified function of the class to be defined,
// have every call site visible, AND (ImportC.cpp:1989) not be externally
// visible unless this TU is the whole program — another TU could call an
// externally visible function with an argument this TU never sees, which
// would break the all-or-nothing per-function promotion rule. UNLIKE
// G1/G2/G8, this gate degrades SILENTLY: the class simply stays on the
// pre-FR-30 Phase-1b slice lowering (`&mut [T]` parameters, `slice_of`
// call sites) — still correct, just unoptimized. This is genuinely a
// suboptimality gate, not a correctness one (contrast with the G4/G5/G6
// cell-slice family, where the analogous restriction currently produces
// a real compile error for the equivalent shape — see
// multi-tu-gate-g5-cellslice-global-external.c).
//
// This file is exactly test/Import/C/owners.c's content (the FR-30
// single-TU positive) plus a trivial companion TU that only forces the
// >=2-TU project import path; `fill`/`sum`/`total`/`main` are already
// externally visible there, so this same source demonstrates the
// fallback with zero changes.
//
// W3.2 (or whichever wave hoists planOwners' call-site visibility to a
// whole-program pre-pass) will flip the CHECK lines below from the
// plain-slice shape to the Owner_main_arr struct/method shape
// test/Import/C/owners.c already asserts for the sole-TU case.
//
// RUN: emitrust-import-c %s %S/Inputs/multi-tu-gate-g3-owner-fallback-other.c | FileCheck %s

void fill(int *p, int n) {
  for (int i = 0; i < n; i++) {
    p[i] = i;
  }
}
// CHECK-LABEL: func.func @fill
// CHECK-SAME: (%{{.*}}: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.*}}: i32)
// CHECK-NOT: emitrust.method_of

int sum(int *p, int n) {
  int s = 0;
  while (n > 0) {
    s = s + *p;
    p++;
    n--;
  }
  return s;
}
// CHECK-LABEL: func.func @sum
// CHECK-SAME: (%{{.*}}: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.*}}: i32) -> i32
// CHECK-NOT: emitrust.method_of

int total(int *p, int n) {
  return sum(p, n);
}
// CHECK-LABEL: func.func @total
// CHECK-NOT: emitrust.method_of
// CHECK: call @sum(
// CHECK-NOT: {{.*}}emitrust.method_call

int main(void) {
  int arr[8];
  fill(arr, 8);
  int t = sum(&arr[2], 4);
  t = t + total(arr, arr[0]);
  return t;
}
// CHECK-LABEL: func.func @c_main
// CHECK: emitrust.slice_of mut
// CHECK: call @fill(
// CHECK: emitrust.slice_of mut
// CHECK: call @sum(
// CHECK: call @total(

// No owner struct is synthesized anywhere in the module.
// CHECK-NOT: emitrust.struct_def @Owner_
// CHECK-NOT: emitrust.method_of
// CHECK-NOT: emitrust.method_call

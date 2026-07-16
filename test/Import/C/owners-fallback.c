// RUN: split-file %s %t
// RUN: emitrust-import-c %t/two-arrays.c | FileCheck %s --check-prefix=TWOARR
// RUN: emitrust-import-c %t/big-array.c | FileCheck %s --check-prefix=BIG
// RUN: emitrust-import-c %t/two-regions.c | FileCheck %s --check-prefix=TWOREG

// FR-30: Phase-4 owner promotion falls back SILENTLY to the Phase-1b slice
// lowering — never a diagnostic — whenever the promotion rule is not met:
// a function reached from two different arrays makes the class multi-base,
// an array above 32 elements exceeds the struct_def Default-derive MVP
// limit, and a function whose pointer parameters resolve into two distinct
// regions fails the all-or-nothing method rule. Each case still imports as
// plain slice-parameter functions with slice_of call sites. The trailing
// NOT checks prove that no owner struct, method attribute, or tagged call
// appears anywhere in the module.

//--- two-arrays.c
int sum(int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    s = s + p[i];
  }
  return s;
}

int main(void) {
  int a[4];
  int b[4];
  a[0] = 1;
  b[0] = 2;
  return sum(a, 4) + sum(b, 4);
}

// One function called on two different arrays: the interprocedural class
// holds two bases, so everything stays on the 1b slice shape.
// TWOARR-LABEL: func.func @sum
// TWOARR-SAME: (%{{.*}}: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.*}}: i32) -> i32
// TWOARR-LABEL: func.func @c_main
// TWOARR: emitrust.slice_of mut %{{.*}} : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
// TWOARR: call @sum(
// TWOARR: emitrust.slice_of mut
// TWOARR: call @sum(
// TWOARR-NOT: emitrust.struct_def @Owner_
// TWOARR-NOT: emitrust.method_of
// TWOARR-NOT: emitrust.method_call

//--- big-array.c
int sum(int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    s = s + p[i];
  }
  return s;
}

int main(void) {
  int arr[64];
  arr[0] = 5;
  return sum(arr, 64);
}

// A 64-element array exceeds the derive(Default) limit of 32.
// BIG-LABEL: func.func @sum
// BIG-SAME: (%{{.*}}: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.*}}: i32) -> i32
// BIG-LABEL: func.func @c_main
// BIG: emitrust.slice_of mut %{{.*}} : (!emitrust.lvalue<!emitrust.array<64xi32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
// BIG: call @sum(
// BIG-NOT: emitrust.struct_def @Owner_
// BIG-NOT: emitrust.method_of
// BIG-NOT: emitrust.method_call

//--- two-regions.c
int dot(int *x, int *y, int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    s = s + x[i] * y[i];
  }
  return s;
}

int main(void) {
  int c[4];
  int d[4];
  c[0] = 3;
  d[0] = 4;
  return dot(c, d, 4);
}

// One function whose two pointer parameters hit two distinct regions:
// neither class may promote (all-or-nothing per function), so both stay
// slices and the call reborrows both bases.
// TWOREG-LABEL: func.func @dot
// TWOREG-SAME: (%{{.*}}: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.*}}: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.*}}: i32) -> i32
// TWOREG-LABEL: func.func @c_main
// TWOREG: emitrust.slice_of mut
// TWOREG: emitrust.slice_of mut
// TWOREG: call @dot(
// TWOREG-NOT: emitrust.struct_def @Owner_
// TWOREG-NOT: emitrust.method_of
// TWOREG-NOT: emitrust.method_call

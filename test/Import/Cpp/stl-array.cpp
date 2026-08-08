// RUN: emitrust-import-c %s | FileCheck %s
// W2.7: std::array<T, N> maps to the SAME `!emitrust.array<NxT>` a C
// `T[N]` maps to — no new opaque family, so every existing array path
// (per-element aggregate init, subscript places in read AND write
// position, multi-type elements) applies unchanged. Pins: the variable's
// mapped type, the one-level struct-wrapper peel of the aggregate
// initializer (per-element subscript/assign exactly like a C array's),
// operator[] lowering to the same `emitrust.subscript` place (behind the
// C++ size_t index conversion, an `emitrust.cast` to ui64), and size()
// folding to the compile-time constant N (index-typed, cast to the call's
// declared C type).

extern "C" int printf(const char *, ...);

#include <array>

// CHECK-LABEL: func.func @sum_array
// CHECK: emitrust.variable named "a" : !emitrust.lvalue<!emitrust.array<3xi32>>
// CHECK: %[[E0:.*]] = arith.constant 0 : i64
// CHECK: emitrust.subscript %{{.*}}[%[[E0]]] : (!emitrust.lvalue<!emitrust.array<3xi32>>, i64) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign
// CHECK: emitrust.subscript
// CHECK: emitrust.assign
// CHECK: emitrust.subscript
// CHECK: emitrust.assign
// CHECK: %[[WIDX:.*]] = emitrust.cast %{{.*}} : i32 to ui64
// CHECK: %[[WPLACE:.*]] = emitrust.subscript %{{.*}}[%[[WIDX]]] : (!emitrust.lvalue<!emitrust.array<3xi32>>, ui64) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[WPLACE]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: %[[N:.*]] = arith.constant 3 : index
// CHECK: emitrust.cast %[[N]] : index to ui64
int sum_array(void) {
  std::array<int, 3> a = {4, 8, 15};
  a[1] = a[0] + 2;
  int n = a.size();
  return a[1] + n;
}

// CHECK-LABEL: func.func @second
// CHECK: emitrust.variable named "d" : !emitrust.lvalue<!emitrust.array<2xf64>>
double second(void) {
  std::array<double, 2> d = {0.5, 2.5};
  return d[1];
}

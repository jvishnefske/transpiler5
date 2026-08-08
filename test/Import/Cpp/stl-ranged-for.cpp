// RUN: emitrust-import-c %s | FileCheck %s
// W2.10: ranged-for over a recognized container local, lowered to a
// len()-bounded counted CFG loop (i64 counter cell; the condition
// re-reads len() every iteration). The R3 soundness argument: the range
// must be a bare local DeclRefExpr and the body may not name the range
// variable (rejection pinned in stl-invalid.cpp's RANGEDFOR split), so
// the container's length is loop-invariant by construction and the
// len()-per-iteration desugar agrees exactly with C++'s
// evaluate-end-once semantics. Pins: the condition's len-cast-compare
// chain; a BY-VALUE loop variable materializing as a fresh per-iteration
// copy in its own place; the `&` form binding the loop variable's symbol
// DIRECTLY to the per-iteration subscript place, so element writes land
// in the container; and the std::array form bounding on the constant N.

extern "C" int printf(const char *, ...);

#include <vector>
#include <array>

// CHECK-LABEL: func.func @sum_vec
// CHECK: %[[I:.*]] = memref.load %{{.*}} : memref<i64>
// CHECK: %[[LEN:.*]] = emitrust.method_call %{{.*}}["len"] () : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>) -> index
// CHECK: %[[LEN64:.*]] = emitrust.cast %[[LEN]] : index to i64
// CHECK: arith.cmpi slt, %[[I]], %[[LEN64]] : i64
// CHECK: cf.cond_br
// CHECK: %[[BI:.*]] = memref.load %{{.*}} : memref<i64>
// CHECK: %[[ELEM:.*]] = emitrust.subscript %{{.*}}[%[[BI]]] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>, i64) -> !emitrust.lvalue<i32>
// CHECK: %[[EV:.*]] = emitrust.load %[[ELEM]]
// CHECK: %[[X:.*]] = emitrust.variable named "x" : !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[X]] = %[[EV]]
int sum_vec(void) {
  std::vector<int> v;
  v.push_back(2);
  v.push_back(5);
  int total = 0;
  for (int x : v) {
    total += x;
  }
  return total;
}

// CHECK-LABEL: func.func @scale_vec
// CHECK: %[[MELEM:.*]] = emitrust.subscript %{{.*}} -> !emitrust.lvalue<i32>
// CHECK-NOT: emitrust.variable named "x"
// CHECK: emitrust.assign %[[MELEM]] = %{{.*}} : !emitrust.lvalue<i32>
int scale_vec(void) {
  std::vector<int> v;
  v.push_back(1);
  for (int &x : v) {
    x = x * 10;
  }
  return v[0];
}

// CHECK-LABEL: func.func @sum_array
// CHECK: %[[N:.*]] = arith.constant 3 : i64
// CHECK: arith.cmpi slt, %{{.*}}, %[[N]] : i64
// CHECK: emitrust.subscript %{{.*}} : (!emitrust.lvalue<!emitrust.array<3xi32>>, i64) -> !emitrust.lvalue<i32>
int sum_array(void) {
  std::array<int, 3> a = {3, 6, 9};
  int total = 0;
  for (int x : a) {
    total += x;
  }
  return total;
}

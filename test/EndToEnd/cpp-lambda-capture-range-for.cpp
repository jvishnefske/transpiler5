// FR-61f B1, the C++ leg -- and specifically the shape that made the
// parameter widening a NEW REJECTION of previously-accepted C++ rather than a
// pure gain, which is why it must be pinned here.
//
// A range-eligible `for` body may not touch anything backed by a
// `memref.alloca` cell: the body is a single-block region, mem2reg cannot
// promote a cell whose load/store lives inside a region op, and
// `convert-to-emitrust` then rejects the leftover alloca with a located
// error. Three binders create such cells -- `emitLocalVar`,
// `bindOrdinaryParam`, and `bindLiftedCaptureValue` -- and each must consult
// `placeBackedScalars`.
//
// The capture binder is the one that is easy to miss, because at HEAD the
// hazard was LATENT: a lambda's own parameter (`k` below) rejected the loop
// first, so a by-value capture's cell could never reach a region. Admitting
// parameters removes that shield and exposes the capture. Without the capture
// binder consulting `placeBackedScalars`, this file fails to compile with
// `failed to legalize operation 'memref.alloca'` -- the SAFE direction (a
// located hard error, never a miscompile), but still a regression against C++
// that used to transpile, so it gets its own byte-diff leg.
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_lambda_capture_range_for > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// Both the by-value CAPTURE and the lambda's own PARAMETER are read in the
// body, so both must have been placed for this loop to lift at all.
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s
// CHECK: for {{_?i}} in 0i32..4i32

#include <cstdio>

int main() {
  int n = 3;
  auto g = [n](int k) {
    int s = 0;
    for (int i = 0; i < 4; i++)
      s += n + k + i;
    return s;
  };
  std::printf("%d\n", g(2));
  return 0;
}

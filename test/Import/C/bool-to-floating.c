// RUN: emitrust-import-c %s | FileCheck %s --implicit-check-not="i1 to f"

// Pins the IR SHAPE of a `_Bool` -> floating conversion: it hops through
// the promoted `i32` (`arith.extui` then `arith.sitofp`) instead of
// feeding the i1 straight into `arith.sitofp`. `--implicit-check-not="i1
// to f"` is the invariant proper -- NO `arith.sitofp %x : i1 to f32/f64`
// may appear anywhere in the module.
//
// Both halves of the hop are load-bearing:
//
//   - `arith.sitofp` on an i1 is a SIGNED widening, so `true` would
//     convert to -1.0 where C mandates 1.0 (C99 6.3.1.4 via 6.3.1.2: a
//     `_Bool` holds exactly 0 or 1 and widens as an unsigned value).
//     `arith.extui` is the same zero-extension the integral-cast path
//     already uses for a width-1 source.
//   - the direct i1 form ALSO had no legal Rust spelling downstream:
//     `ArithToEmitRust`'s CastOpConversion rendered it as `bool as f64`,
//     which rustc refuses outright (E0606), so the emitted crate could
//     not be built at all. See test/EndToEnd/bool-to-floating.c for the
//     runtime oracle.
//
// This is exactly the shape the usual arithmetic conversions already
// produced for `b * 2.5` (clang puts a `_Bool` -> `int` node in front of
// the multiply), which is why that expression was correct while the plain
// conversions were not; the two paths now agree.

// CHECK-LABEL: func.func @implicit
double implicit(int n) {
  _Bool b = n > 0;
  // CHECK: %[[B:.*]] = memref.load %alloca[] : memref<i1>
  // CHECK-NEXT: %[[EXT:.*]] = arith.extui %[[B]] : i1 to i32
  // CHECK-NEXT: arith.sitofp %[[EXT]] : i32 to f64
  return b;
}

// CHECK-LABEL: func.func @explicit_float
float explicit_float(int n) {
  _Bool b = n > 0;
  // CHECK: %[[BF:.*]] = memref.load %alloca[] : memref<i1>
  // CHECK-NEXT: %[[EXTF:.*]] = arith.extui %[[BF]] : i1 to i32
  // CHECK-NEXT: arith.sitofp %[[EXTF]] : i32 to f32
  return (float)b;
}

// The usual-arithmetic-conversion path: clang already inserts the
// `_Bool` -> `int` node, so this shape is UNCHANGED and pinned as a
// negative control -- one hop, not two.
// CHECK-LABEL: func.func @promoted
double promoted(int n) {
  _Bool b = n > 0;
  // CHECK: %[[BP:.*]] = memref.load %alloca[] : memref<i1>
  // CHECK-NEXT: %[[EXTP:.*]] = arith.extui %[[BP]] : i1 to i32
  // CHECK-NEXT: %[[FP:.*]] = arith.sitofp %[[EXTP]] : i32 to f64
  // CHECK: arith.mulf %[[FP]]
  return b * 2.5;
}

// An ordinary signed int source keeps the single `arith.sitofp` -- the
// hop is added for i1 only.
// CHECK-LABEL: func.func @int_source
// CHECK-NOT: arith.extui
// CHECK: arith.sitofp %{{.*}} : i32 to f64
double int_source(int n) { return n; }

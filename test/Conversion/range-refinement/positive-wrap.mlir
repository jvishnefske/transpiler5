// FR: checker soundness under wraparound — unsigned arithmetic lowers to
// wrapping_* in Rust, so the checker's transfer functions must model WRAP
// semantics on both sides. A transfer function that wrongly assumed nsw/nuw
// (treating overflow as poison/empty) would derive an empty or disjoint
// pre-conversion range here and emit a false-positive diagnostic. With
// wrap-correct transfer functions the pre and post ranges agree ([0, 0] for
// the ui32 flow, [-2147483648, -2147483648] for the i32 flow) and the module
// must PASS with no diagnostic.
//
// RUN: emitrust-opt --emitrust-range-refinement-check %s | FileCheck %s

// ui32 flow through the unsigned-typed emitrust path: 4294967295 + 1 wraps
// to 0. This is exactly the IR shape emitrust-import-c produces for
// `unsigned int w = 4294967295u + 1u;` after mem2reg+canonicalize.
// CHECK-LABEL: func.func @wrap_u32
// CHECK: emitrust.add
// CHECK: emitrust.call_opaque "print!"
func.func @wrap_u32() -> i32 {
  %c0_i32 = arith.constant 0 : i32
  %umax = emitrust.constant <4294967295 : ui32> : ui32
  %one = emitrust.constant <1 : ui32> : ui32
  // wraps: [0, 0], NOT empty/poison
  %w = emitrust.add %umax, %one : ui32
  emitrust.call_opaque "print!"(%w) {args = ["{}\0A", 0 : index]} : (ui32) -> ()
  return %c0_i32 : i32
}

// i32 flow on the arith side: INT_MAX + 1 wraps to INT_MIN (arith ops carry
// no overflow flags here, so two's-complement wrap is the defined result).
// CHECK-LABEL: func.func @wrap_i32
func.func @wrap_i32() -> i32 {
  %cmax = arith.constant 2147483647 : i32
  %c1_i32 = arith.constant 1 : i32
  // wraps: [-2147483648, -2147483648]
  %w = arith.addi %cmax, %c1_i32 : i32
  emitrust.call_opaque "print!"(%w) {args = ["{}\0A", 0 : index]} : (i32) -> ()
  return %w : i32
}

// FR: pipeline differential CONCRETE interpretation — the value-identity
// checker complements the range-refinement checker, which catches sign/width
// divergence but is blind to VALUE-IDENTITY bugs: the project's worst
// historical miscompile class (lost-copy yield rebinding — two values with
// identical ranges, the wrong one observed). This mode interprets BOTH
// stages concretely and compares exact observed values.
//
// PRIMARY input mode: a plain post-lift pre-convert module (the same stage
// --emitrust-range-refinement-check runs at in emitrust-cc). The pass
// concretely INTERPRETS every function that takes NO arguments (in practice
// c_main and parameterless helpers), internally clones + runs
// convert-to-emitrust, interprets the clone identically, and compares in
// execution order:
//   * every emitrust.call_opaque "print!" operand VALUE (exact integer
//     equality), keyed by (function, occurrence index, operand index)
//   * each function's return value(s)
//
// Coverage exercised here (all must interpret, both sides): arith constant/
// addi/cmpi/select, a bounded canonical scf.while + condition/yield,
// emitrust.constant/add on ui32 (WRAP-free small values), emitrust.variable/
// assign/load scalar places, func.call between interpreted functions.
// Identical observed values on both sides: no diagnostic, exit 0, and the
// pass is analysis-only so the input module round-trips unchanged.
//
// RUN: emitrust-opt --emitrust-value-identity-check %s | FileCheck %s

// CHECK-LABEL: func.func @helper
// CHECK: emitrust.call_opaque "print!"
// CHECK: return
func.func @helper() -> i32 {
  %c8_i32 = arith.constant 8 : i32
  %c3_i32 = arith.constant 3 : i32
  %c5_i32 = arith.constant 5 : i32
  emitrust.call_opaque "print!"(%c3_i32) {args = ["{}\0A", 0 : index]} : (i32) -> ()
  emitrust.call_opaque "print!"(%c5_i32) {args = ["{}\0A", 0 : index]} : (i32) -> ()
  return %c8_i32 : i32
}

// CHECK-LABEL: func.func @c_main
// CHECK: scf.while
// CHECK: emitrust.variable
// CHECK: call @helper
// CHECK: return
func.func @c_main() -> i32 {
  %c0_i32 = arith.constant 0 : i32
  %c1_i32 = arith.constant 1 : i32
  %c10_i32 = arith.constant 10 : i32
  %c100_i32 = arith.constant 100 : i32

  // Straight-line: cmp + select. inc = 1, cond = true, sel = 1.
  %inc = arith.addi %c0_i32, %c1_i32 : i32
  %cond = arith.cmpi slt, %inc, %c100_i32 : i32
  %sel = arith.select %cond, %inc, %c100_i32 : i32
  emitrust.call_opaque "print!"(%sel) {args = ["{}\0A", 0 : index]} : (i32) -> ()

  // Bounded canonical scf.while: sum of 0..9 = 45.
  %res:2 = scf.while (%i = %c0_i32, %sum = %c0_i32) : (i32, i32) -> (i32, i32) {
    %c = arith.cmpi slt, %i, %c10_i32 : i32
    scf.condition(%c) %i, %sum : i32, i32
  } do {
  ^bb0(%i: i32, %sum: i32):
    %nsum = arith.addi %sum, %i : i32
    %ni = arith.addi %i, %c1_i32 : i32
    scf.yield %ni, %nsum : i32, i32
  }

  // Scalar place round-trip: assign the loop sum, load it back (45).
  %v = emitrust.variable : !emitrust.lvalue<i32>
  emitrust.assign %v = %res#1 : !emitrust.lvalue<i32>
  %l = emitrust.load %v : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "print!"(%l) {args = ["{}\0A", 0 : index]} : (i32) -> ()

  // emitrust-typed straight line: 7u + 1u = 8u.
  %u7 = emitrust.constant <7 : ui32> : ui32
  %u1 = emitrust.constant <1 : ui32> : ui32
  %u8 = emitrust.add %u7, %u1 : ui32
  emitrust.call_opaque "print!"(%u8) {args = ["{}\0A", 0 : index]} : (ui32) -> ()

  // Interpreted-to-interpreted call: helper prints 3, 5 and returns 8.
  %h = func.call @helper() : () -> i32
  emitrust.call_opaque "print!"(%h) {args = ["{}\0A", 0 : index]} : (i32) -> ()
  return %c0_i32 : i32
}

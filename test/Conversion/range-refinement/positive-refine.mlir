// FR: pipeline differential abstract interpretation — the range-refinement
// checker re-derives integer ranges before and after convert-to-emitrust and
// accepts a module whose observed ranges overlap at every observation point.
//
// PRIMARY input mode: a plain post-mem2reg+canonicalize module. The pass
// analyzes it, internally clones + runs convert-to-emitrust, re-analyzes the
// clone, and matches observation points:
//   * operands of emitrust.call_opaque "print!", keyed by
//     (enclosing function name, occurrence index of the call_opaque within
//     that function, operand index)
//   * function return operands, keyed by
//     (function name, occurrence index of the return within the function,
//     operand index)
//
// Tight, fully computable ranges on both sides: no diagnostic, exit 0, and
// the pass is analysis-only so the input module round-trips unchanged.
//
// RUN: emitrust-opt --emitrust-range-refinement-check %s | FileCheck %s

// CHECK-LABEL: func.func @refine
// CHECK: arith.andi
// CHECK: arith.select
// CHECK: emitrust.call_opaque "print!"
// CHECK: return
func.func @refine(%arg0: i32) -> i32 {
  %c1_i32 = arith.constant 1 : i32
  %c100_i32 = arith.constant 100 : i32
  %c255_i32 = arith.constant 255 : i32
  // [0, 255]
  %masked = arith.andi %arg0, %c255_i32 : i32
  // [1, 256]
  %inc = arith.addi %masked, %c1_i32 : i32
  // [0, 1]
  %cond = arith.cmpi slt, %inc, %c100_i32 : i32
  // [1, 256]
  %sel = arith.select %cond, %inc, %c100_i32 : i32
  emitrust.call_opaque "print!"(%sel) {args = ["{}\0A", 0 : index]} : (i32) -> ()
  emitrust.call_opaque "print!"(%cond) {args = ["{}\0A", 0 : index]} : (i1) -> ()
  return %sel : i32
}

// A second function pins per-function matching (occurrence indices restart).
// CHECK-LABEL: func.func @refine2
func.func @refine2(%arg0: i32) -> i32 {
  %c7_i32 = arith.constant 7 : i32
  // [0, 7]
  %masked = arith.andi %arg0, %c7_i32 : i32
  // [0, 49]
  %sq = arith.muli %masked, %masked : i32
  emitrust.call_opaque "print!"(%sq) {args = ["{}\0A", 0 : index]} : (i32) -> ()
  return %sq : i32
}

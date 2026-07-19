// FR: the interpreter carries a step BUDGET per entry point so adversarial
// IR cannot hang the checker; exhausting it aborts to UNSUPPORTED-COVERAGE
// (soft skip), never an error. The budget must be GENEROUS, not
// trigger-happy: loops in generated code always terminate, and a
// few-thousand-iteration terminating loop is everyday staged IR. This test
// pins ONLY that such a loop still verifies in bounded time (exit 0, no
// diagnostic, module round-trips) — no numeric budget value is pinned.
//
// sum of 0..4095 = 8386560 on both sides; any value-identity divergence
// introduced by conversion of the loop back-edge (the lost-copy class)
// would be observed at the print.
//
// RUN: emitrust-opt --emitrust-value-identity-check %s | FileCheck %s

// CHECK-LABEL: func.func @c_main
// CHECK: scf.while
// CHECK: emitrust.call_opaque "print!"
// CHECK: return
func.func @c_main() -> i32 {
  %c0_i32 = arith.constant 0 : i32
  %c1_i32 = arith.constant 1 : i32
  %c4096_i32 = arith.constant 4096 : i32
  %res:2 = scf.while (%i = %c0_i32, %sum = %c0_i32) : (i32, i32) -> (i32, i32) {
    %c = arith.cmpi slt, %i, %c4096_i32 : i32
    scf.condition(%c) %i, %sum : i32, i32
  } do {
  ^bb0(%i: i32, %sum: i32):
    %nsum = arith.addi %sum, %i : i32
    %ni = arith.addi %i, %c1_i32 : i32
    scf.yield %ni, %nsum : i32, i32
  }
  emitrust.call_opaque "print!"(%res#1) {args = ["{}\0A", 0 : index]} : (i32) -> ()
  return %c0_i32 : i32
}

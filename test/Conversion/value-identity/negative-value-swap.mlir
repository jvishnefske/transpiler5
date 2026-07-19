// FR: HARD ERROR on value divergence at a print! observation point — the
// exact LOST-COPY shape (yield rebinding) that motivated this checker.
// Both stages hold a=3 and b=5 in scalar places; the "after" stage prints
// them SWAPPED (b then a). Every value flows through a memory load, so the
// range-refinement checker sees TOP on both sides at every observation
// point (nonempty intersection, silent PASS): ranges are IDENTICAL, only
// the concrete VALUES differ. The value-identity checker interprets both
// stages concretely and must flag the first diverging print occurrence.
//
// SECONDARY input mode (mirrors the range checker): a module holding
// exactly two nested builtin.module ops tagged emitrust.stage = "before" /
// "after"; the pass skips its internal conversion and compares the pair
// directly. Entry points are the parameterless functions.
//
// Diagnostic contract (BINDING wording, values fully pinned):
//   error: value identity violation: '<fn>' print operand <k> at
//   occurrence <n> observed <pre-value> pre-conversion but <post-value>
//   post-conversion
//
// RUN: not emitrust-opt --emitrust-value-identity-check %s 2>&1 | FileCheck %s

// CHECK: error: value identity violation: 'f' print operand 0 at occurrence 0 observed 3 pre-conversion but 5 post-conversion
module {
  builtin.module attributes {emitrust.stage = "before"} {
    func.func @f() -> i32 {
      %c0_i32 = arith.constant 0 : i32
      %c3_i32 = arith.constant 3 : i32
      %c5_i32 = arith.constant 5 : i32
      %a = emitrust.variable : !emitrust.lvalue<i32>
      %b = emitrust.variable : !emitrust.lvalue<i32>
      emitrust.assign %a = %c3_i32 : !emitrust.lvalue<i32>
      emitrust.assign %b = %c5_i32 : !emitrust.lvalue<i32>
      %la = emitrust.load %a : (!emitrust.lvalue<i32>) -> i32
      %lb = emitrust.load %b : (!emitrust.lvalue<i32>) -> i32
      // Correct order: 3 then 5.
      emitrust.call_opaque "print!"(%la) {args = ["{}\0A", 0 : index]} : (i32) -> ()
      emitrust.call_opaque "print!"(%lb) {args = ["{}\0A", 0 : index]} : (i32) -> ()
      return %c0_i32 : i32
    }
  }
  builtin.module attributes {emitrust.stage = "after"} {
    emitrust.func @f() -> i32 {
      %c0_i32 = emitrust.constant <0 : i32> : i32
      %c3_i32 = emitrust.constant <3 : i32> : i32
      %c5_i32 = emitrust.constant <5 : i32> : i32
      %a = emitrust.variable : !emitrust.lvalue<i32>
      %b = emitrust.variable : !emitrust.lvalue<i32>
      emitrust.assign %a = %c3_i32 : !emitrust.lvalue<i32>
      emitrust.assign %b = %c5_i32 : !emitrust.lvalue<i32>
      %la = emitrust.load %a : (!emitrust.lvalue<i32>) -> i32
      %lb = emitrust.load %b : (!emitrust.lvalue<i32>) -> i32
      // Lost-copy miscompile: 5 then 3. Same ranges (TOP), wrong values.
      emitrust.call_opaque "print!"(%lb) {args = ["{}\0A", 0 : index]} : (i32) -> ()
      emitrust.call_opaque "print!"(%la) {args = ["{}\0A", 0 : index]} : (i32) -> ()
      emitrust.return %c0_i32 : i32
    }
  }
}

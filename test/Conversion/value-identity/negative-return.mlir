// FR: function returns are observation points too — HARD ERROR on value
// divergence at a RETURN. The "before" stage returns 42 loaded from a
// scalar place (range TOP), the "after" stage returns the constant 41
// (range [41, 41]). The ranges INTERSECT, so the range-refinement checker
// is silent — only concrete interpretation observes 42 != 41. The print!
// operands of both stages agree (7) so the ONLY violation is the return
// value.
//
// Diagnostic contract (BINDING wording, values fully pinned; twin of the
// print-operand form):
//   error: value identity violation: '<fn>' return value <k> at
//   occurrence <n> observed <pre-value> pre-conversion but <post-value>
//   post-conversion
//
// RUN: not emitrust-opt --emitrust-value-identity-check %s 2>&1 | FileCheck %s

// CHECK: error: value identity violation: 'g' return value 0 at occurrence 0 observed 42 pre-conversion but 41 post-conversion
module {
  builtin.module attributes {emitrust.stage = "before"} {
    func.func @g() -> i32 {
      %c7_i32 = arith.constant 7 : i32
      %c42_i32 = arith.constant 42 : i32
      %v = emitrust.variable : !emitrust.lvalue<i32>
      emitrust.assign %v = %c42_i32 : !emitrust.lvalue<i32>
      %l = emitrust.load %v : (!emitrust.lvalue<i32>) -> i32
      emitrust.call_opaque "print!"(%c7_i32) {args = ["{}\0A", 0 : index]} : (i32) -> ()
      return %l : i32
    }
  }
  builtin.module attributes {emitrust.stage = "after"} {
    emitrust.func @g() -> i32 {
      %c7_i32 = emitrust.constant <7 : i32> : i32
      // Miscompiled: returns 41, not the stored 42.
      %bad = emitrust.constant <41 : i32> : i32
      emitrust.call_opaque "print!"(%c7_i32) {args = ["{}\0A", 0 : index]} : (i32) -> ()
      emitrust.return %bad : i32
    }
  }
}

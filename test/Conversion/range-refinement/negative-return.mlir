// FR: function returns are observation points too — HARD ERROR on empty
// intersection at a RETURN. The "before" return value is provably in
// [0, 255]; the "after" module returns a constant -1, i.e. [-1, -1],
// disjoint under both signed and unsigned interpretation. The print!
// operands of both stages agree ([7, 7]) so the ONLY violation is the
// return value.
//
// Diagnostic contract (BINDING wording, bounds intentionally unpinned):
//   error: range refinement violation: '<fn>' return value <k> has
//   pre-conversion range [..] disjoint from post-conversion range [..]
//
// RUN: not emitrust-opt --emitrust-range-refinement-check %s 2>&1 | FileCheck %s

// CHECK: error: range refinement violation: 'g' return value 0 has pre-conversion range
// CHECK-SAME: disjoint from post-conversion range
module {
  builtin.module attributes {emitrust.stage = "before"} {
    func.func @g(%arg0: i32) -> i32 {
      %c7_i32 = arith.constant 7 : i32
      %c255_i32 = arith.constant 255 : i32
      // [0, 255]
      %masked = arith.andi %arg0, %c255_i32 : i32
      emitrust.call_opaque "print!"(%c7_i32) {args = ["{}\0A", 0 : index]} : (i32) -> ()
      return %masked : i32
    }
  }
  builtin.module attributes {emitrust.stage = "after"} {
    emitrust.func @g(%arg0: i32) -> i32 {
      %c7_i32 = emitrust.constant <7 : i32> : i32
      // [-1, -1]: disjoint from [0, 255]
      %bad = emitrust.constant <-1 : i32> : i32
      emitrust.call_opaque "print!"(%c7_i32) {args = ["{}\0A", 0 : index]} : (i32) -> ()
      emitrust.return %bad : i32
    }
  }
}

// FR: HARD ERROR on empty intersection at a print! observation point.
// The "before" module's print operand is provably in [200, 255]
// (andi 255 then ori 200); the "after" module models a miscompiled
// sign-extension by constant-folding the same operand to -56, i.e.
// [-56, -56]. The ranges are disjoint under both signed and unsigned
// interpretation, so the pass must emit a located error and exit nonzero.
// The returns of both stages agree ([0, 0]) so the ONLY violation is the
// print operand.
//
// Diagnostic contract (BINDING wording, bounds intentionally unpinned):
//   error: range refinement violation: '<fn>' print operand <k> has
//   pre-conversion range [..] disjoint from post-conversion range [..]
//
// RUN: not emitrust-opt --emitrust-range-refinement-check %s 2>&1 | FileCheck %s

// CHECK: error: range refinement violation: 'f' print operand 0 has pre-conversion range
// CHECK-SAME: disjoint from post-conversion range
module {
  builtin.module attributes {emitrust.stage = "before"} {
    func.func @f(%arg0: i32) -> i32 {
      %c0_i32 = arith.constant 0 : i32
      %c200_i32 = arith.constant 200 : i32
      %c255_i32 = arith.constant 255 : i32
      // [0, 255]
      %masked = arith.andi %arg0, %c255_i32 : i32
      // [200, 255]
      %v = arith.ori %masked, %c200_i32 : i32
      emitrust.call_opaque "print!"(%v) {args = ["{}\0A", 0 : index]} : (i32) -> ()
      return %c0_i32 : i32
    }
  }
  builtin.module attributes {emitrust.stage = "after"} {
    emitrust.func @f(%arg0: i32) -> i32 {
      %c0_i32 = emitrust.constant <0 : i32> : i32
      // [-56, -56]: disjoint from [200, 255]
      %bad = emitrust.constant <-56 : i32> : i32
      emitrust.call_opaque "print!"(%bad) {args = ["{}\0A", 0 : index]} : (i32) -> ()
      emitrust.return %c0_i32 : i32
    }
  }
}

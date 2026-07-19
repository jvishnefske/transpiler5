// FR: containment failure is NOT an error — only EMPTY INTERSECTION is.
// Memory loads (emitrust.load; there is no pre-conversion memref stage in
// this pipeline, the importer emits emitrust places directly) and
// call_opaque results stay TOP. Here the "before" print operand and return
// value are provably in [0, 255], while the "after" side feeds the same
// observation points through an emitrust.load, so its range is TOP:
// post is NOT contained in pre, but the intersection is [0, 255] != empty,
// so the checker must PASS with no diagnostic.
//
// SECONDARY input mode: a module holding exactly two nested builtin.module
// ops tagged emitrust.stage = "before" / "after"; the pass skips its
// internal conversion and compares the pair directly.
//
// RUN: emitrust-opt --emitrust-range-refinement-check %s | FileCheck %s

// CHECK: emitrust.stage = "before"
// CHECK-LABEL: func.func @mem
// CHECK: emitrust.stage = "after"
// CHECK-LABEL: emitrust.func @mem
module {
  builtin.module attributes {emitrust.stage = "before"} {
    func.func @mem(%arg0: i32) -> i32 {
      %c255_i32 = arith.constant 255 : i32
      // [0, 255]
      %masked = arith.andi %arg0, %c255_i32 : i32
      emitrust.call_opaque "print!"(%masked) {args = ["{}\0A", 0 : index]} : (i32) -> ()
      return %masked : i32
    }
  }
  builtin.module attributes {emitrust.stage = "after"} {
    emitrust.func @mem(%arg0: i32) -> i32 {
      %v = emitrust.variable : !emitrust.lvalue<i32>
      // TOP: memory loads are not refined
      %l = emitrust.load %v : (!emitrust.lvalue<i32>) -> i32
      emitrust.call_opaque "print!"(%l) {args = ["{}\0A", 0 : index]} : (i32) -> ()
      emitrust.return %l : i32
    }
  }
}

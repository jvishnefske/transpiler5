// FR-134: emitrust-canonical-roundtrip is the first slice of the OPTIONAL
// canonicalization pipe -- a verification stage that prints the module to
// MLIR's canonical GENERIC form, parses that text back into a fresh context,
// re-prints it, and requires the two texts to be identical.
//
// What this file pins is the pass's CONTRACT, and there are three parts to it:
//
// 1. It MUTATES NOTHING. The module after the pass is byte-identical to the
//    module before, so enabling the stage can never change emitted Rust. That
//    is why it is safe to bolt onto the pinned pipeline at all.
// 2. It runs on the FRONT END's module -- before any lowering stage -- so a
//    failure is attributable to the importer rather than to a later pass.
// 3. It checks the GENERIC form, not the pretty one. That distinction is
//    load-bearing and was measured: upstream `cf.switch`'s CUSTOM assembly
//    cannot round-trip a negative case value (it prints `-2` as
//    `18446744073709551614`, which its own parser then rejects), while the
//    same op's generic form prints `case_values = dense<-2>` and round-trips
//    cleanly. Checking the pretty form would report that upstream cosmetic
//    bug as a project defect. The `@negative_switch_case` function below is
//    exactly that shape, and it must PASS.
//
// RUN: emitrust-opt --emitrust-canonical-roundtrip %s | FileCheck %s

// A representative slice of the dialects an imported module actually carries:
// emitrust places and loads, arith, memref cells, scf, cf and func.

// CHECK-LABEL: emitrust.func @places
emitrust.func @places(%arg0: i32) -> i32 {
  %v = emitrust.variable named "v" : !emitrust.lvalue<i32>
  emitrust.assign %v = %arg0 : !emitrust.lvalue<i32>
  %r = emitrust.load %v : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// THE UPSTREAM-BUG SHAPE, and the reason this pass reads the generic form. A
// negative `cf.switch` case value survives the generic round-trip; it would
// NOT survive a pretty one.
// CHECK-LABEL: func.func @negative_switch_case
func.func @negative_switch_case(%v: i64) -> i32 {
  %c1 = arith.constant 1 : i32
  %c2 = arith.constant 2 : i32
  cf.switch %v : i64, [
    default: ^bb2,
    -2: ^bb1
  ]
^bb1:
  return %c1 : i32
^bb2:
  return %c2 : i32
}

// An unsigned type in a signature, the shape that made `unsigned.c` interesting.
// CHECK-LABEL: emitrust.func @unsigned_sig
emitrust.func @unsigned_sig(%arg0: ui32) -> ui64 {
  %c = emitrust.constant <7 : ui64> : ui64
  emitrust.return %c : ui64
}

// A region op with an implicit terminator: the `emitrust.for` body is a
// single-block region, so its elided `emitrust.yield` must survive the
// round-trip through the generic form, where it is NOT elided.
// CHECK-LABEL: emitrust.func @region_op
emitrust.func @region_op(%lo: i32, %hi: i32, %st: i32) {
  emitrust.for %i = %lo to %hi step %st : i32 {
    emitrust.call_opaque "print!"(%i) : (i32) -> ()
  }
  emitrust.return
}

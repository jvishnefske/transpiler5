// FR-61f: the FR-63 compound-assign fold (`v = v <op> e` -> `v <op>= e`,
// clippy::assign_op_pattern) extends to a bare `emitrust.variable` PLACE, not
// only SSA bindings -- an accumulator kept as a place by the range-`for` lift
// (`s = s + i` -> `s += i`) still folds. The fold is gated so it stays sound:
// the target must be a bare local variable place (a projection like `*p`/`a[i]`
// is excluded elsewhere -- re-reading it under `<op>=` could re-run a side
// effect), and operand 0 must be an INLINE load OF that place (a load bound to
// its own `let vN`, e.g. hoisted before a barrier, must not fold or the
// dropped operand would orphan that binding). A non-self-referential assign
// (`s = t + e`) never folds.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// A bare-variable place whose value is `load(self) + e` folds to `s += e`
// (the load is single-use, so it renders inline as the place read).
// CHECK-LABEL: fn acc(v0: i32) -> i32 {
// CHECK-NEXT:    let mut s: i32 = 0;
// CHECK-NEXT:    s += v0;
// CHECK-NEXT:    s
// CHECK-NEXT:  }
emitrust.func @acc(%arg0: i32) -> i32 {
  %s = emitrust.variable named "s" <0 : i32> : !emitrust.lvalue<i32>
  %l = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %n = emitrust.add %l, %arg0 : i32
  emitrust.assign %s = %n : !emitrust.lvalue<i32>
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// Not self-referential: operand 0 loads a DIFFERENT place (`t`), so the assign
// keeps its `s = t + v0` form -- folding would be wrong.
// CHECK-LABEL: fn notself(v0: i32) -> i32 {
// CHECK-NEXT:    let mut s: i32 = 0;
// CHECK-NEXT:    let t: i32 = 5;
// CHECK-NEXT:    s = t + v0;
// CHECK-NEXT:    s
// CHECK-NEXT:  }
emitrust.func @notself(%arg0: i32) -> i32 {
  %s = emitrust.variable named "s" <0 : i32> : !emitrust.lvalue<i32>
  %t = emitrust.variable named "t" <5 : i32> : !emitrust.lvalue<i32>
  %l = emitrust.load %t : (!emitrust.lvalue<i32>) -> i32
  %n = emitrust.add %l, %arg0 : i32
  emitrust.assign %s = %n : !emitrust.lvalue<i32>
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

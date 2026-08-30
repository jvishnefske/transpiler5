// FR-152: the MECHANISM of the escaping-loop-place fix, pinned at IR level so
// the two halves of it are separately visible.
//
// Half one is this pass: an `emitrust.variable` place declared inside an
// `scf.while` region whose result reaches the loop's own `scf.condition` (or
// leaves the loop entirely) is moved IN FRONT of the loop. Nothing else
// changes -- the assign and the loads stay exactly where they were, because a
// place is only a declaration.
//
// Half two is machinery that already existed: with the declaration outside,
// the carried value is loop-invariant, so the canonicalizer that already
// follows this pass in `buildLoweringPipeline` deletes it through MLIR's own
// `RemoveLoopInvariantArgsFromBeforeBlock` / `RemoveLoopInvariantValueYielded`
// and the `scf.while` result arity shrinks from 2 to 1. That is what makes the
// lvalue-typed loop result -- which `SCFToEmitRust`'s `WhileLowering` cannot
// default-initialize -- disappear before the conversion ever sees it. No new
// op is needed anywhere, which is exactly why this fix is a pass and not a
// dialect change.
//
// The negative legs matter as much as the positive one: an `init` attribute
// must NOT be hoisted (that is the FR-155 miscompile -- the per-iteration
// reset rides on the op and would leave the loop with it), a `const` place
// must not be hoisted, and a place that never escapes must be left alone so no
// existing golden moves.
//
// RUN: emitrust-opt %s --emitrust-hoist-escaping-loop-places \
// RUN:   | FileCheck %s --check-prefix=HOIST
// RUN: emitrust-opt %s --emitrust-hoist-escaping-loop-places --canonicalize \
// RUN:   | FileCheck %s --check-prefix=CANON

// HOIST-LABEL: func.func @escapes
// The declaration is now before the loop, and the loop still carries it.
// HOIST:       emitrust.variable named "c"
// HOIST-NEXT:  scf.while
// HOIST:       scf.condition
//
// CANON-LABEL: func.func @escapes
// CANON:       emitrust.variable named "c"
// The carried lvalue is gone: one operand, one result, one block argument.
// CANON:       scf.while (%{{.*}} = %{{.*}}) : (i32) -> i32
// CANON-NOT:   !emitrust.lvalue<ui8>) -> ()
func.func @escapes(%arg0: i32) -> i32 attributes {emitrust.param_names = ["n"]} {
  %c1_i32 = arith.constant 1 : i32
  %c0_i32 = arith.constant 0 : i32
  %0:2 = scf.while (%arg1 = %arg0) : (i32) -> (i32, !emitrust.lvalue<ui8>) {
    %3 = emitrust.variable named "c" : !emitrust.lvalue<ui8>
    %4 = emitrust.cast %arg1 : i32 to ui8
    emitrust.assign %3 = %4 : !emitrust.lvalue<ui8>
    %5 = arith.addi %arg1, %c1_i32 : i32
    %6 = emitrust.load %3 : (!emitrust.lvalue<ui8>) -> ui8
    %7 = emitrust.cast %6 : ui8 to i32
    %8 = arith.cmpi ne, %7, %c0_i32 : i32
    scf.condition(%8) %5, %3 : i32, !emitrust.lvalue<ui8>
  } do {
  ^bb0(%arg1: i32, %arg2: !emitrust.lvalue<ui8>):
    scf.yield %arg1 : i32
  }
  %1 = emitrust.load %0#1 : (!emitrust.lvalue<ui8>) -> ui8
  %2 = emitrust.cast %1 : ui8 to i32
  return %2 : i32
}

// A place that stays inside the loop is untouched: it is not carried, so
// nothing needs moving and moving it would be a gratuitous golden shift.
// HOIST-LABEL: func.func @stays
// HOIST:       scf.while
// HOIST:       emitrust.variable named "t"
func.func @stays(%arg0: i32) -> i32 {
  %c1_i32 = arith.constant 1 : i32
  %c0_i32 = arith.constant 0 : i32
  %0 = scf.while (%arg1 = %arg0) : (i32) -> i32 {
    %1 = emitrust.variable named "t" : !emitrust.lvalue<ui8>
    %2 = emitrust.cast %arg1 : i32 to ui8
    emitrust.assign %1 = %2 : !emitrust.lvalue<ui8>
    %3 = emitrust.load %1 : (!emitrust.lvalue<ui8>) -> ui8
    %4 = emitrust.cast %3 : ui8 to i32
    %5 = arith.cmpi ne, %4, %c0_i32 : i32
    %6 = arith.addi %arg1, %c1_i32 : i32
    scf.condition(%5) %6 : i32
  } do {
  ^bb0(%arg1: i32):
    scf.yield %arg1 : i32
  }
  return %0 : i32
}

// An escaping place that carries an `init` attribute is NOT hoisted: the
// initializer rides on the op, so hoisting would carry the per-iteration reset
// out of the loop with the declaration and silently change the answer (the
// measured FR-155 defect). It stays inside the loop and stays a rejection.
// HOIST-LABEL: func.func @has_init
// HOIST:       scf.while
// HOIST:       emitrust.variable named "k" <7 : i32>
func.func @has_init(%arg0: i32) -> i32 {
  %c1_i32 = arith.constant 1 : i32
  %c0_i32 = arith.constant 0 : i32
  %0:2 = scf.while (%arg1 = %arg0) : (i32) -> (i32, !emitrust.lvalue<i32>) {
    %3 = emitrust.variable named "k" <7 : i32> : !emitrust.lvalue<i32>
    %5 = arith.addi %arg1, %c1_i32 : i32
    %8 = arith.cmpi ne, %arg1, %c0_i32 : i32
    scf.condition(%8) %5, %3 : i32, !emitrust.lvalue<i32>
  } do {
  ^bb0(%arg1: i32, %arg2: !emitrust.lvalue<i32>):
    scf.yield %arg1 : i32
  }
  %1 = emitrust.load %0#1 : (!emitrust.lvalue<i32>) -> i32
  return %1 : i32
}

// A `const` place cannot be assigned after its declaration at all, so hoisting
// it away from its initializer is never meaningful. Left alone.
// HOIST-LABEL: func.func @is_const
// HOIST:       scf.while
// HOIST:       emitrust.variable named "k" const <7 : i32>
func.func @is_const(%arg0: i32) -> i32 {
  %c1_i32 = arith.constant 1 : i32
  %c0_i32 = arith.constant 0 : i32
  %0:2 = scf.while (%arg1 = %arg0) : (i32) -> (i32, !emitrust.lvalue<i32>) {
    %3 = emitrust.variable named "k" const <7 : i32> : !emitrust.lvalue<i32>
    %5 = arith.addi %arg1, %c1_i32 : i32
    %8 = arith.cmpi ne, %arg1, %c0_i32 : i32
    scf.condition(%8) %5, %3 : i32, !emitrust.lvalue<i32>
  } do {
  ^bb0(%arg1: i32, %arg2: !emitrust.lvalue<i32>):
    scf.yield %arg1 : i32
  }
  %1 = emitrust.load %0#1 : (!emitrust.lvalue<i32>) -> i32
  return %1 : i32
}

// The FENCE is not a blanket refusal to touch droppy types: a place whose type
// carries `emitrust.has_drop` but which never escapes its loop is left alone
// SILENTLY. Only an escaping one is an error (pinned in
// hoist-escaping-loop-places-invalid.mlir and, end to end with its wording, in
// test/Driver/loop-escaping-place-drop-reject.cpp).
// HOIST-LABEL: func.func @drop_stays
// HOIST:       scf.while
// HOIST:       emitrust.variable named "r"
emitrust.struct_def @R ["id"] [i32] {emitrust.has_drop}

func.func @drop_stays(%arg0: i32) -> i32 {
  %c1_i32 = arith.constant 1 : i32
  %c0_i32 = arith.constant 0 : i32
  %0 = scf.while (%arg1 = %arg0) : (i32) -> i32 {
    %1 = emitrust.variable named "r" : !emitrust.lvalue<!emitrust.struct<"R">>
    %2 = emitrust.member %1["id"] : (!emitrust.lvalue<!emitrust.struct<"R">>) -> !emitrust.lvalue<i32>
    emitrust.assign %2 = %arg1 : !emitrust.lvalue<i32>
    %3 = arith.cmpi ne, %arg1, %c0_i32 : i32
    %4 = arith.addi %arg1, %c1_i32 : i32
    scf.condition(%3) %4 : i32
  } do {
  ^bb0(%arg1: i32):
    scf.yield %arg1 : i32
  }
  return %0 : i32
}

// Nested loops need a FIXPOINT: hoisting the inner place lands it in the OUTER
// loop's region, where the same rule applies again. One pass run must reach
// the function's entry block, not stop one level short.
// HOIST-LABEL: func.func @nested
// HOIST:       emitrust.variable named "c"
// HOIST-NEXT:  scf.while
// HOIST:       scf.while
func.func @nested(%arg0: i32) -> i32 {
  %c1_i32 = arith.constant 1 : i32
  %c0_i32 = arith.constant 0 : i32
  %0:2 = scf.while (%arg1 = %arg0) : (i32) -> (i32, !emitrust.lvalue<ui8>) {
    %10:2 = scf.while (%arg2 = %arg1) : (i32) -> (i32, !emitrust.lvalue<ui8>) {
      %3 = emitrust.variable named "c" : !emitrust.lvalue<ui8>
      %4 = emitrust.cast %arg2 : i32 to ui8
      emitrust.assign %3 = %4 : !emitrust.lvalue<ui8>
      %5 = arith.addi %arg2, %c1_i32 : i32
      %8 = arith.cmpi ne, %arg2, %c0_i32 : i32
      scf.condition(%8) %5, %3 : i32, !emitrust.lvalue<ui8>
    } do {
    ^bb0(%arg2: i32, %arg3: !emitrust.lvalue<ui8>):
      scf.yield %arg2 : i32
    }
    %9 = arith.cmpi ne, %10#0, %c0_i32 : i32
    scf.condition(%9) %10#0, %10#1 : i32, !emitrust.lvalue<ui8>
  } do {
  ^bb0(%arg1: i32, %arg2: !emitrust.lvalue<ui8>):
    scf.yield %arg1 : i32
  }
  %1 = emitrust.load %0#1 : (!emitrust.lvalue<ui8>) -> ui8
  %2 = emitrust.cast %1 : ui8 to i32
  return %2 : i32
}

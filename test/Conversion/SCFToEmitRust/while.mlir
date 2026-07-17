// FR: convert-scf-to-emitrust lowers scf.while to emitrust.loop with
// mutable lets for the carried values and results, and an exit through
// emitrust.if + emitrust.break on the negated condition.
// RUN: emitrust-opt --convert-scf-to-emitrust %s | FileCheck %s

// CHECK-LABEL: func.func @count_up
// CHECK:         %[[CARRIED:.*]] = emitrust.let mut %arg0 : i32
// CHECK:         %[[DEF:.*]] = emitrust.constant <0 : i32> : i32
// CHECK:         %[[OUT:.*]] = emitrust.let mut %[[DEF]] : i32
// CHECK:         emitrust.loop {
// CHECK:           %[[COND:.*]] = emitrust.cmp lt, %[[CARRIED]], %arg1 : (i32, i32) -> i1
// CHECK:           %[[FALSE:.*]] = emitrust.constant <false> : i1
// CHECK:           %[[NEG:.*]] = emitrust.cmp eq, %[[COND]], %[[FALSE]] : (i1, i1) -> i1
// CHECK:           emitrust.if %[[NEG]] {
// CHECK-NEXT:        emitrust.assign %[[OUT]] = %[[CARRIED]] : i32
// CHECK-NEXT:        emitrust.break
// CHECK-NEXT:      }
// CHECK:           %[[ONE:.*]] = emitrust.constant <1 : i32> : i32
// CHECK:           %[[NEXT:.*]] = emitrust.add %[[CARRIED]], %[[ONE]] : i32
// CHECK:           emitrust.assign %[[CARRIED]] = %[[NEXT]] : i32
// CHECK:         }
// CHECK:         return %[[OUT]] : i32
// CHECK-NOT:     scf.while
func.func @count_up(%init: i32, %limit: i32) -> i32 {
  %0 = scf.while (%v = %init) : (i32) -> i32 {
    %cond = emitrust.cmp lt, %v, %limit : (i32, i32) -> i1
    scf.condition(%cond) %v : i32
  } do {
  ^bb0(%w: i32):
    %one = emitrust.constant <1 : i32> : i32
    %next = emitrust.add %w, %one : i32
    scf.yield %next : i32
  }
  return %0 : i32
}

// The back-edge of a swapping while loop is a copy cycle: rebinding the
// carried lets sequentially would read a clobbered value (the lost-copy
// problem), so the yield sources are staged into immutable temporaries
// before any carried let is assigned.
// CHECK-LABEL: func.func @swap_carried
// CHECK:         %[[A:.*]] = emitrust.let mut %arg0 : i32
// CHECK:         %[[B:.*]] = emitrust.let mut %arg1 : i32
// CHECK:         emitrust.loop {
// CHECK:           %[[TA:.*]] = emitrust.let %[[B]] : i32
// CHECK-NEXT:      %[[TB:.*]] = emitrust.let %[[A]] : i32
// CHECK-NEXT:      emitrust.assign %[[A]] = %[[TA]] : i32
// CHECK-NEXT:      emitrust.assign %[[B]] = %[[TB]] : i32
// CHECK:         }
// CHECK-NOT:     scf.while
func.func @swap_carried(%ia: i32, %ib: i32, %limit: i32) -> (i32, i32) {
  %0:2 = scf.while (%a = %ia, %b = %ib) : (i32, i32) -> (i32, i32) {
    %cond = emitrust.cmp lt, %a, %limit : (i32, i32) -> i1
    scf.condition(%cond) %a, %b : i32, i32
  } do {
  ^bb0(%x: i32, %y: i32):
    scf.yield %y, %x : i32, i32
  }
  return %0#0, %0#1 : i32, i32
}

// A hazard-free back-edge keeps the direct assignment form: the added
// value is a fresh SSA binding computed before the rebinding, so no
// temporaries are staged even though a carried let is also a source read
// before it is written.
// CHECK-LABEL: func.func @fib_carried
// CHECK:         %[[FA:.*]] = emitrust.let mut %arg0 : i32
// CHECK:         %[[FB:.*]] = emitrust.let mut %arg1 : i32
// CHECK:         emitrust.loop {
// CHECK:           %[[SUM:.*]] = emitrust.add %[[FA]], %[[FB]] : i32
// CHECK-NEXT:      emitrust.assign %[[FA]] = %[[FB]] : i32
// CHECK-NEXT:      emitrust.assign %[[FB]] = %[[SUM]] : i32
// CHECK:         }
// CHECK-NOT:     scf.while
func.func @fib_carried(%ia: i32, %ib: i32, %limit: i32) -> (i32, i32) {
  %0:2 = scf.while (%a = %ia, %b = %ib) : (i32, i32) -> (i32, i32) {
    %cond = emitrust.cmp lt, %b, %limit : (i32, i32) -> i1
    scf.condition(%cond) %a, %b : i32, i32
  } do {
  ^bb0(%x: i32, %y: i32):
    %next = emitrust.add %x, %y : i32
    scf.yield %y, %next : i32, i32
  }
  return %0#0, %0#1 : i32, i32
}

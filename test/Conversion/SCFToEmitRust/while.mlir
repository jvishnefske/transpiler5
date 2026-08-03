// FR-61c: convert-scf-to-emitrust lifts a pure-condition scf.while to
// emitrust.while -- the before-region becomes the condition region (folded
// into the Rust `while` head by the emitter), the after-region becomes the
// body ending in the carried-let backedge assignments, and the loop
// results are the carried lets themselves: no result lets, no exit copies,
// no tail break. A loop whose condition chain is impure (loads, calls) or
// whose forwarded values are computed inside the loop keeps the
// emitrust.loop lowering with the negated-condition exit.
// RUN: emitrust-opt --convert-scf-to-emitrust %s | FileCheck %s

// CHECK-LABEL: func.func @count_up
// CHECK:         %[[CARRIED:.*]] = emitrust.let mut %arg0 : i32
// CHECK:         emitrust.while {
// CHECK-NEXT:      %[[COND:.*]] = emitrust.cmp lt, %[[CARRIED]], %arg1 : (i32, i32) -> i1
// CHECK-NEXT:      emitrust.condition %[[COND]]
// CHECK-NEXT:    } do {
// CHECK-NEXT:      %[[ONE:.*]] = emitrust.constant <1 : i32> : i32
// CHECK-NEXT:      %[[NEXT:.*]] = emitrust.add %[[CARRIED]], %[[ONE]] : i32
// CHECK-NEXT:      emitrust.assign %[[CARRIED]] = %[[NEXT]] : i32
// CHECK:         }
// CHECK:         return %[[CARRIED]] : i32
// CHECK-NOT:     scf.while
// CHECK-NOT:     emitrust.break
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

// The lift-cf-to-scf CANONICAL form: the loop body lives in the
// before-region as a cond-guarded scf.if whose else-arm yields the exit
// defaults, the after-region forwards. The lift splits it: the pure prefix
// becomes the condition region, the then-arm plus the after ops become the
// body, and the else-arm yields become the loop's replacement values.
// CHECK-LABEL: func.func @canonical_body_if
// CHECK:         %[[CI:.*]] = emitrust.let mut %arg0 : i32
// CHECK:         emitrust.while {
// CHECK-NEXT:      %[[CC:.*]] = emitrust.cmp lt, %[[CI]], %arg1 : (i32, i32) -> i1
// CHECK-NEXT:      emitrust.condition %[[CC]]
// CHECK-NEXT:    } do {
// CHECK-NEXT:      %[[STEP:.*]] = emitrust.constant <2 : i32> : i32
// CHECK-NEXT:      %[[CN:.*]] = emitrust.add %[[CI]], %[[STEP]] : i32
// CHECK-NEXT:      emitrust.assign %[[CI]] = %[[CN]] : i32
// CHECK:         }
// The exit value is the else-arm yield: the carried let, not an exit copy.
// CHECK:         return %[[CI]] : i32
// CHECK-NOT:     scf.while
// CHECK-NOT:     emitrust.break
func.func @canonical_body_if(%init: i32, %limit: i32) -> i32 {
  %0 = scf.while (%v = %init) : (i32) -> i32 {
    %cond = emitrust.cmp lt, %v, %limit : (i32, i32) -> i1
    %body = scf.if %cond -> (i32) {
      %step = emitrust.constant <2 : i32> : i32
      %next = emitrust.add %v, %step : i32
      scf.yield %next : i32
    } else {
      scf.yield %v : i32
    }
    scf.condition(%cond) %body : i32
  } do {
  ^bb0(%w: i32):
    scf.yield %w : i32
  }
  return %0 : i32
}

// The back-edge of a swapping while loop is a copy cycle: rebinding the
// carried lets sequentially would read a clobbered value (the lost-copy
// problem), so the yield sources are staged into immutable temporaries
// before any carried let is assigned -- inside the while body exactly as
// they were inside the loop.
// CHECK-LABEL: func.func @swap_carried
// CHECK:         %[[A:.*]] = emitrust.let mut %arg0 : i32
// CHECK:         %[[B:.*]] = emitrust.let mut %arg1 : i32
// CHECK:         emitrust.while {
// CHECK:           emitrust.condition
// CHECK-NEXT:    } do {
// CHECK-NEXT:      %[[TA:.*]] = emitrust.let %[[B]] : i32
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
// CHECK:         emitrust.while {
// CHECK:           emitrust.condition
// CHECK-NEXT:    } do {
// CHECK-NEXT:      %[[SUM:.*]] = emitrust.add %[[FA]], %[[FB]] : i32
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

// NOT lifted: an impure condition chain (a load feeds the comparison)
// keeps the emitrust.loop lowering -- and idiomatic Rust for a
// side-effecting condition IS `loop { .. break }`.
// CHECK-LABEL: func.func @impure_condition
// CHECK:         emitrust.loop {
// CHECK:           %[[FALSE:.*]] = emitrust.constant <false> : i1
// CHECK:           %[[NEG:.*]] = emitrust.cmp eq, %{{.*}}, %[[FALSE]] : (i1, i1) -> i1
// CHECK:           emitrust.if %[[NEG]] {
// CHECK:             emitrust.break
// CHECK:           }
// CHECK:         }
// CHECK-NOT:     emitrust.while
// CHECK-NOT:     scf.while
func.func @impure_condition(%cell: memref<i32>, %limit: i32) -> i32 {
  %0 = scf.while (%v = %limit) : (i32) -> i32 {
    %current = memref.load %cell[] : memref<i32>
    %cond = emitrust.cmp lt, %current, %limit : (i32, i32) -> i1
    scf.condition(%cond) %v : i32
  } do {
  ^bb0(%w: i32):
    scf.yield %w : i32
  }
  return %0 : i32
}

// NOT lifted: a condition argument computed inside the before-region
// would be unnameable after a `while`; the loop lowering's exit copies
// handle it.
// CHECK-LABEL: func.func @computed_exit_value
// CHECK:         emitrust.loop {
// CHECK:           emitrust.break
// CHECK:         }
// CHECK-NOT:     emitrust.while
// CHECK-NOT:     scf.while
func.func @computed_exit_value(%init: i32, %limit: i32) -> i32 {
  %0 = scf.while (%v = %init) : (i32) -> i32 {
    %two = emitrust.constant <2 : i32> : i32
    %doubled = emitrust.mul %v, %two : i32
    %cond = emitrust.cmp lt, %doubled, %limit : (i32, i32) -> i1
    scf.condition(%cond) %doubled : i32
  } do {
  ^bb0(%w: i32):
    scf.yield %w : i32
  }
  return %0 : i32
}

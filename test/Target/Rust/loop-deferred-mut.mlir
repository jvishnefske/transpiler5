// FR-105 (defect): the `mut` of a DEFERRED binding (`let j: T;` with the
// initializer dropped) written inside a loop. `loopReassign` decides it, and
// its own comment states an ANY-path intent -- "a write can recur when a body
// path that writes the binding loops back" -- while the old implementation
// asked `writtenAtExit`, which is an ALL-path property ("every fall-through
// path has written it"). A lifted C `for` becomes `loop { if cond { ..write.. }
// if !cond { break } }`, so the write sits under a condition, a non-writing
// path exists, and the `mut` was dropped: the emitted crate then fails to
// build with `error[E0384]: cannot assign twice to immutable variable`.
//
// This pins the corrected predicate, which is EXACT rather than merely
// conservative -- emitted crates deny `unused_mut`, so an unneeded `mut` is a
// hard build failure exactly as a missing one is. `mut` iff SOME body path
// writes the binding and reaches the BACK EDGE, by falling through the body or
// by `continue`. Every arm below was checked against rustc itself by flipping
// the emitter's choice: flipping a `mut` off gives E0384, flipping one on gives
// "variable does not need to be mutable". The bare-`let` arms are therefore
// regression pins of equal standing with the `mut` arms, not mere leftovers.
//
// FR-132 note: the three arms whose ONLY write is in the SAME BLOCK as the
// declaration now render merged (`let j: i32 = v0;` instead of `let j: i32;`
// followed by `j = v0;`). What those arms pin is unchanged and unweakened --
// the ABSENCE of `mut` -- and the merged spelling pins it just as hard, since
// a wrongly-added `mut` would read `let mut j: i32 = v0;`. Every arm whose
// write is inside a loop or `if` region keeps the bare declaration, because
// sinking it into the region would move the binding out of scope.
//
// The ANY-write state is loop-SCOPED: it is reseeded false at every loop-body
// entry, so a write from BEFORE a loop can never be attributed to that loop's
// back edge (@write_before_nested_loops is that guard -- the naive unscoped
// form of this fix marks it `mut` and breaks a crate that builds today).
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// ---------------------------------------------------------------------------
// The defect itself, on all three loop ops.
// ---------------------------------------------------------------------------

// The FR-105 minimal reproducer in dialect form: a `loop {}` (the lifted C
// `for`) whose body writes `j` under an `if` and reads it only afterwards, so
// the init defers. The write reaches the back edge on the next iteration --
// `mut`.
// CHECK-LABEL: fn cond_write_backedge(v0: i32, v1: bool, v2: bool) {
// CHECK-NEXT:    let mut j: i32;
emitrust.func @cond_write_backedge(%arg0: i32, %arg1: i1, %arg2: i1) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.if %arg1 {
      emitrust.assign %j = %arg0 : !emitrust.lvalue<i32>
      %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
      emitrust.call_opaque "sink"(%l) : (i32) -> ()
    }
    emitrust.if %arg2 {
      emitrust.break
    }
  }
  emitrust.return
}

// The same shape under an `emitrust.for` RANGE head (the FR-61f counting-loop
// lift). The defect is not specific to the `loop {}` lift; all three loop ops
// share one predicate.
// CHECK-LABEL: fn for_cond_write(v0: usize, v1: usize, v2: usize, v3: bool, v4: i32) {
// CHECK-NEXT:    let mut j: i32;
emitrust.func @for_cond_write(%lo: index, %hi: index, %st: index, %c: i1, %v: i32) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.for %i = %lo to %hi step %st {
    emitrust.if %c {
      emitrust.assign %j = %v : !emitrust.lvalue<i32>
      %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
      emitrust.call_opaque "sink"(%l) : (i32) -> ()
    }
  }
  emitrust.return
}

// And under `emitrust.while`, whose condition region is analyzed with the same
// loop-scoped seed as the body.
// CHECK-LABEL: fn while_cond_write(v0: bool, v1: bool, v2: i32) {
// CHECK-NEXT:    let mut j: i32;
emitrust.func @while_cond_write(%c: i1, %d: i1, %v: i32) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.while {
    emitrust.condition %c
  } do {
    emitrust.if %d {
      emitrust.assign %j = %v : !emitrust.lvalue<i32>
      %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
      emitrust.call_opaque "sink"(%l) : (i32) -> ()
    }
    emitrust.yield
  }
  emitrust.return
}

// `continue` is a back edge too. The written path never falls through the body
// -- it jumps to the loop head -- and the write still recurs, so the rule is
// "reaches the back edge", not "falls through".
// CHECK-LABEL: fn cond_write_continue(v0: i32, v1: bool, v2: bool) {
// CHECK-NEXT:    let mut j: i32;
emitrust.func @cond_write_continue(%arg0: i32, %arg1: i1, %arg2: i1) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.if %arg1 {
      emitrust.assign %j = %arg0 : !emitrust.lvalue<i32>
      %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
      emitrust.call_opaque "sink"(%l) : (i32) -> ()
      emitrust.continue
    }
    emitrust.if %arg2 {
      emitrust.break
    }
  }
  emitrust.return
}

// A `switch` arm that writes and falls out of the match reaches the back edge
// just as an `if` arm does.
// CHECK-LABEL: fn switch_arm_write_falls(v0: i32, v1: i32, v2: bool) {
// CHECK-NEXT:    let mut j: i32;
emitrust.func @switch_arm_write_falls(%v: i32, %d: i32, %c: i1) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.switch %d : i32
    case 0 {
      emitrust.assign %j = %v : !emitrust.lvalue<i32>
      %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
      emitrust.call_opaque "sink"(%l) : (i32) -> ()
    }
    default {
    }
    emitrust.if %c {
      emitrust.break
    }
  }
  emitrust.return
}

// Nested `if`s do not hide the write: any fall-through path out of the body
// counts.
// CHECK-LABEL: fn deep_nested_write(v0: i32, v1: bool, v2: bool, v3: bool) {
// CHECK-NEXT:    let mut j: i32;
emitrust.func @deep_nested_write(%v: i32, %a: i1, %b: i1, %c: i1) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.if %a {
      emitrust.if %b {
        emitrust.assign %j = %v : !emitrust.lvalue<i32>
        %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
        emitrust.call_opaque "sink"(%l) : (i32) -> ()
      }
    }
    emitrust.if %c {
      emitrust.break
    }
  }
  emitrust.return
}

// The write reaches the INNER loop's back edge via `continue`; the outer body
// then breaks unconditionally. The inner recurrence alone requires `mut`, so
// the outer loop's unconditional break cannot excuse it.
// CHECK-LABEL: fn inner_continue_outer_break(v0: i32, v1: bool, v2: bool) {
// CHECK-NEXT:    let mut j: i32;
emitrust.func @inner_continue_outer_break(%v: i32, %c: i1, %d: i1) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.loop {
      emitrust.if %c {
        emitrust.assign %j = %v : !emitrust.lvalue<i32>
        %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
        emitrust.call_opaque "sink"(%l) : (i32) -> ()
        emitrust.continue
      }
      emitrust.if %d {
        emitrust.break
      }
    }
    emitrust.break
  }
  emitrust.return
}

// A write-then-break inside an inner loop whose EXIT falls through to the OUTER
// back edge does recur, once per outer iteration.
// CHECK-LABEL: fn inner_break_outer_backedge(v0: i32, v1: bool, v2: bool) {
// CHECK-NEXT:    let mut j: i32;
emitrust.func @inner_break_outer_backedge(%arg0: i32, %arg1: i1, %arg2: i1) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.loop {
      emitrust.if %arg1 {
        emitrust.assign %j = %arg0 : !emitrust.lvalue<i32>
        %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
        emitrust.call_opaque "sink"(%l) : (i32) -> ()
        emitrust.break
      }
    }
    emitrust.if %arg2 {
      emitrust.break
    }
  }
  emitrust.return
}

// ---------------------------------------------------------------------------
// The `mut` shapes that were ALREADY correct and must not move.
// ---------------------------------------------------------------------------

// An unconditional body write with a conditional break: caught before the fix
// by the ALL-path term, and by the ANY-path term after it.
// CHECK-LABEL: fn uncond_write_loop(v0: i32, v1: bool) -> i32 {
// CHECK-NEXT:    let mut j: i32;
emitrust.func @uncond_write_loop(%arg0: i32, %arg1: i1) -> i32 {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.assign %j = %arg0 : !emitrust.lvalue<i32>
    emitrust.if %arg1 {
      emitrust.break
    }
  }
  %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %l : i32
}

// The do/while shape: write, read, then a tail-conditional break.
// CHECK-LABEL: fn do_while_write(v0: i32, v1: bool) {
// CHECK-NEXT:    let mut j: i32;
emitrust.func @do_while_write(%v: i32, %c: i1) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.assign %j = %v : !emitrust.lvalue<i32>
    %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
    emitrust.call_opaque "sink"(%l) : (i32) -> ()
    emitrust.if %c {
      emitrust.break
    }
  }
  emitrust.return
}

// ---------------------------------------------------------------------------
// The bare-`let` pins: a write that never reaches a back edge. `deny(unused_mut)`
// makes each of these a build failure if the predicate over-marks.
// ---------------------------------------------------------------------------

// Write then `break` in the same arm: the back edge is unreachable from the
// write, so the assignment happens at most once.
// CHECK-LABEL: fn cond_write_then_break(v0: i32, v1: bool) {
// CHECK-NEXT:    let j: i32;
emitrust.func @cond_write_then_break(%arg0: i32, %arg1: i1) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.if %arg1 {
      emitrust.assign %j = %arg0 : !emitrust.lvalue<i32>
      %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
      emitrust.call_opaque "sink"(%l) : (i32) -> ()
      emitrust.break
    }
  }
  emitrust.return
}

// The write falls through its `if`, but an UNCONDITIONAL break at the body tail
// ends the iteration before the back edge.
// CHECK-LABEL: fn write_then_tail_break(v0: i32, v1: bool) {
// CHECK-NEXT:    let j: i32;
emitrust.func @write_then_tail_break(%v: i32, %c: i1) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.if %c {
      emitrust.assign %j = %v : !emitrust.lvalue<i32>
      %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
      emitrust.call_opaque "sink"(%l) : (i32) -> ()
    }
    emitrust.break
  }
  emitrust.return
}

// Divergence after the write (a `return`/`panic!` tail) is not a back edge.
// CHECK-LABEL: fn write_then_diverge(v0: i32, v1: bool) {
// CHECK-NEXT:    let j: i32;
emitrust.func @write_then_diverge(%v: i32, %c: i1) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.if %c {
      emitrust.assign %j = %v : !emitrust.lvalue<i32>
      %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
      emitrust.call_opaque "sink"(%l) : (i32) -> ()
      emitrust.call_opaque "panic!"() : () -> ()
    }
  }
  emitrust.return
}

// The `switch` counterpart: every arm breaks out of the loop, so no arm reaches
// the back edge.
// CHECK-LABEL: fn switch_arm_write_break(v0: i32, v1: i32) {
// CHECK-NEXT:    let j: i32;
emitrust.func @switch_arm_write_break(%v: i32, %d: i32) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.switch %d : i32
    case 0 {
      emitrust.assign %j = %v : !emitrust.lvalue<i32>
      %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
      emitrust.call_opaque "sink"(%l) : (i32) -> ()
      emitrust.break
    }
    default {
      emitrust.break
    }
  }
  emitrust.return
}

// Write-then-break in an inner loop whose exit runs into an UNCONDITIONAL break
// of the outer body: neither back edge is ever reached.
// CHECK-LABEL: fn inner_write_outer_break(v0: i32, v1: bool) {
// CHECK-NEXT:    let j: i32;
emitrust.func @inner_write_outer_break(%v: i32, %c: i1) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.loop {
      emitrust.if %c {
        emitrust.assign %j = %v : !emitrust.lvalue<i32>
        %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
        emitrust.call_opaque "sink"(%l) : (i32) -> ()
        emitrust.break
      }
    }
    emitrust.break
  }
  emitrust.return
}

// The `for` range head with a write-then-break arm.
// CHECK-LABEL: fn for_write_break(v0: usize, v1: usize, v2: usize, v3: bool, v4: i32) {
// CHECK-NEXT:    let j: i32;
emitrust.func @for_write_break(%lo: index, %hi: index, %st: index, %c: i1, %v: i32) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.for %i = %lo to %hi step %st {
    emitrust.if %c {
      emitrust.assign %j = %v : !emitrust.lvalue<i32>
      %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
      emitrust.call_opaque "sink"(%l) : (i32) -> ()
      emitrust.break
    }
  }
  emitrust.return
}

// THE OVER-MARKING GUARD, and the one shape where the obvious implementation of
// this fix is wrong. `j`'s ONLY write is BEFORE both loops; the inner loop just
// breaks. If the break/continue recorders looked at the ALL-path `written`
// state instead of the loop-scoped ANY-path one, that pre-loop write would leak
// out of the inner loop into the outer body and mark `j` mutable -- and this
// function, which builds today, would fail with `error: variable does not need
// to be mutable` under the emitted crate's own `deny(unused_mut)`.
// CHECK-LABEL: fn write_before_nested_loops(v0: i32, v1: bool, v2: bool) {
// CHECK-NEXT:    let j: i32 = v0;
emitrust.func @write_before_nested_loops(%arg0: i32, %arg1: i1, %arg2: i1) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.assign %j = %arg0 : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.loop {
      emitrust.if %arg1 {
        emitrust.break
      }
    }
    %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
    emitrust.call_opaque "sink"(%l) : (i32) -> ()
    emitrust.if %arg2 {
      emitrust.break
    }
  }
  emitrust.return
}

// The same guard on the `continue` recorder: the pre-loop write must not be
// attributed to a `continue` that merely passes through it.
// CHECK-LABEL: fn write_before_continue(v0: i32, v1: bool, v2: bool) {
// CHECK-NEXT:    let j: i32 = v0;
emitrust.func @write_before_continue(%arg0: i32, %arg1: i1, %arg2: i1) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.assign %j = %arg0 : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.if %arg1 {
      emitrust.continue
    }
    %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
    emitrust.call_opaque "sink"(%l) : (i32) -> ()
    emitrust.if %arg2 {
      emitrust.break
    }
  }
  emitrust.return
}

// A single write before a loop that only READS the binding: the plainest form
// of the same guard.
// CHECK-LABEL: fn write_before_loop_read_in(v0: i32, v1: bool) {
// CHECK-NEXT:    let j: i32 = v0;
emitrust.func @write_before_loop_read_in(%v: i32, %c: i1) {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.assign %j = %v : !emitrust.lvalue<i32>
  emitrust.loop {
    %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
    emitrust.call_opaque "sink"(%l) : (i32) -> ()
    emitrust.if %c {
      emitrust.break
    }
  }
  emitrust.return
}

// A binding READ after the loop is never deferred at all: the post-loop read
// makes the initializer live, so it keeps its `= 0` and its `mut`. Pinned
// because it is the shape that looks like the defect and is not -- an FR-105
// test written this way would silently fail to reproduce anything.
// CHECK-LABEL: fn cond_write_read_after_loop(v0: i32, v1: bool, v2: bool) -> i32 {
// CHECK-NEXT:    let mut j: i32 = 0;
emitrust.func @cond_write_read_after_loop(%arg0: i32, %arg1: i1, %arg2: i1) -> i32 {
  %j = emitrust.variable named "j" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.if %arg1 {
      emitrust.assign %j = %arg0 : !emitrust.lvalue<i32>
    }
    emitrust.if %arg2 {
      emitrust.break
    }
  }
  %l = emitrust.load %j : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %l : i32
}

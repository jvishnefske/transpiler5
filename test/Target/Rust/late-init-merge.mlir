// FR-132 (clippy::needless_late_init): the LATE-INIT MERGE. A deferred binding
// renders at TWO program points -- `let s: i32;` at its declaration and
// `s = <rhs>;` at its initializing write. This pins the fold of those two
// points into ONE `let [mut] s: i32 = <rhs>;` rendered at the ASSIGN, with the
// declaration rendering nothing at all.
//
// Why this is a RENDERING change and not the forbidden liveness reasoning:
// `computeDeferredInits` has ALREADY proved that nothing reads the binding
// between the declaration and that first write (that proof is what licensed
// dropping the initializer in the first place -- rustc E0381 would reject the
// current output otherwise). A declaration that emits no code, sunk past
// statements that provably cannot observe it, is inert. No store is dropped,
// no expression moves, no evaluation order changes.
//
// The binding condition is SCOPE, not adjacency: intervening pure statements
// are the COMMON case (the motivating shape from test/EndToEnd/switch-enum.c
// is `let s: i32; let v10: i32 = classify(c, i); s = v10;`, and an
// "immediately following" rule misses 19% of the corpus's candidates). The
// initializing write must be in the SAME BLOCK, must write the binding WHOLE
// and DIRECTLY, and must be the first surviving write.
//
// The refusal legs carry the correctness, and each is pinned below:
//   * a first write inside a region op (`if`/`match`/`loop` arm) -- sinking the
//     declaration INTO the region would move the binding out of scope for the
//     reads that follow the region;
//   * any mention of the binding in the gap -- a read, a projection, or a
//     nested write -- even one that renders nothing;
//   * DROP ORDER: Rust drops in reverse DECLARATION order, so sinking a
//     may-drop declaration past ANOTHER may-drop declaration flips two
//     destructors. Measured with rustc -O on the two hand-written orderings of
//     one program: `dtor 1; dtor 2` became `dtor 2; dtor 1`, and BOTH crates
//     compiled clean -- exactly the miscompile class `cargo build` cannot see.
//     C++ cannot reach this shape today (both source routes are located
//     rejections), so it is pinned here on hand-written IR, with a scalar twin
//     of the identical shape proving the gate -- not the fold -- is what
//     refuses;
//   * FR-61b's if-expression binding keeps PRIORITY where both apply.
//
// The declaration's name is claimed even when it renders nothing (exactly as
// `emitIfExprBinding` does), so v-numbering is unchanged corpus-wide. If a vNN
// in any golden shifts, that unconditional claim was lost -- that is a bug, not
// churn, and no CHECK may be relaxed to absorb it.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

emitrust.struct_def @L ["id"] [i32] {emitrust.has_drop}
emitrust.struct_def @P ["x", "y"] [i32, i32]

// ---------------------------------------------------------------------------
// The win.
// ---------------------------------------------------------------------------

// THE MOTIVATING SHAPE: two pure statements sit between the declaration and
// its initializing write. Both keep their position and their order; only the
// `let s: i32;` line disappears, reappearing as the `let` of the assign.
// CHECK-LABEL: fn gapped(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: i32 = classify(v0);
// CHECK-NEXT:    println!("i={}", v0);
// CHECK-NEXT:    let s: i32 = v1;
// CHECK-NEXT:    s + s
// CHECK-NEXT:  }
emitrust.func @gapped(%arg0: i32) -> i32 {
  %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
  %v = emitrust.call_opaque "classify"(%arg0) : (i32) -> i32
  emitrust.call_opaque "println!"(%arg0) {args = ["i={}", 0 : index]} : (i32) -> ()
  emitrust.assign %s = %v : !emitrust.lvalue<i32>
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %r2 = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %sum = emitrust.add %r, %r2 : i32
  emitrust.return %sum : i32
}

// The degenerate zero-gap case.
// CHECK-LABEL: fn adjacent(v0: i32) -> i32 {
// CHECK-NEXT:    let s: i32 = v0;
// CHECK-NEXT:    s + s
// CHECK-NEXT:  }
emitrust.func @adjacent(%arg0: i32) -> i32 {
  %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
  emitrust.assign %s = %arg0 : !emitrust.lvalue<i32>
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %r2 = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %sum = emitrust.add %r, %r2 : i32
  emitrust.return %sum : i32
}

// A `mut` binding merges too -- `let mut acc: i32 = v1;` followed by a later
// reassignment is correct Rust. `deferredInits`'s mapped bool stays the single
// source of truth for `mut`, so the surviving reassignment keeps it (emitted
// crates deny(unused_mut), so a stale `mut` is a hard build failure).
// CHECK-LABEL: fn mut_merge(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: i32 = helper(v0);
// CHECK-NEXT:    let mut acc: i32 = v1;
// CHECK-NEXT:    let v2: i32 = acc;
// CHECK-NEXT:    println!("{}", v2);
// CHECK-NEXT:    let v3: i32 = helper(v2);
// CHECK-NEXT:    acc = v3;
// CHECK-NEXT:    acc
// CHECK-NEXT:  }
emitrust.func @mut_merge(%arg0: i32) -> i32 {
  %acc = emitrust.variable named "acc" : !emitrust.lvalue<i32>
  %v = emitrust.call_opaque "helper"(%arg0) : (i32) -> i32
  emitrust.assign %acc = %v : !emitrust.lvalue<i32>
  %r = emitrust.load %acc : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "println!"(%r) {args = ["{}", 0 : index]} : (i32) -> ()
  %u = emitrust.call_opaque "helper"(%r) : (i32) -> i32
  emitrust.assign %acc = %u : !emitrust.lvalue<i32>
  %r2 = emitrust.load %acc : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r2 : i32
}

// A DEAD store precedes the real initializing write. It renders nothing, so
// the scan steps over it and merges at the surviving write -- and the binding
// must NOT gain a spurious `mut` from the store that was eliminated.
// CHECK-LABEL: fn dead_store_first(_v0: i32) -> i32 {
// CHECK-NEXT:    let x: i32 = 3i32;
// CHECK-NEXT:    x + x
// CHECK-NEXT:  }
emitrust.func @dead_store_first(%arg0: i32) -> i32 {
  %x = emitrust.variable named "x" : !emitrust.lvalue<i32>
  emitrust.assign %x = %arg0 : !emitrust.lvalue<i32>
  %c = emitrust.constant <3 : i32> : i32
  emitrust.assign %x = %c : !emitrust.lvalue<i32>
  %r = emitrust.load %x : (!emitrust.lvalue<i32>) -> i32
  %r2 = emitrust.load %x : (!emitrust.lvalue<i32>) -> i32
  %sum = emitrust.add %r, %r2 : i32
  emitrust.return %sum : i32
}

// Two deferred bindings declared together and initialized in the OPPOSITE
// order. Each merges at its own write, so the emitted declaration order is the
// initialization order -- which is why the drop-order gate below exists.
// CHECK-LABEL: fn interleaved(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: i32 = helper(v0);
// CHECK-NEXT:    let b: i32 = v1;
// CHECK-NEXT:    let v2: i32 = b;
// CHECK-NEXT:    let v3: i32 = helper(v2);
// CHECK-NEXT:    let a: i32 = v3;
// CHECK-NEXT:    a + v2
// CHECK-NEXT:  }
emitrust.func @interleaved(%arg0: i32) -> i32 {
  %a = emitrust.variable named "a" : !emitrust.lvalue<i32>
  %b = emitrust.variable named "b" : !emitrust.lvalue<i32>
  %tb = emitrust.call_opaque "helper"(%arg0) : (i32) -> i32
  emitrust.assign %b = %tb : !emitrust.lvalue<i32>
  %rb = emitrust.load %b : (!emitrust.lvalue<i32>) -> i32
  %ta = emitrust.call_opaque "helper"(%rb) : (i32) -> i32
  emitrust.assign %a = %ta : !emitrust.lvalue<i32>
  %ra = emitrust.load %a : (!emitrust.lvalue<i32>) -> i32
  %sum = emitrust.add %ra, %rb : i32
  emitrust.return %sum : i32
}

// The FIRST write is in the same block; a LATER one is inside a loop region.
// Only the first write's position matters, so this merges -- and the loop's
// reassignment is what makes it `mut`.
// CHECK-LABEL: fn loop_reassign_after_merge(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: i32 = helper(v0);
// CHECK-NEXT:    let mut acc: i32 = v1;
// CHECK-NEXT:    loop {
emitrust.func @loop_reassign_after_merge(%arg0: i32) -> i32 {
  %acc = emitrust.variable named "acc" : !emitrust.lvalue<i32>
  %t = emitrust.call_opaque "helper"(%arg0) : (i32) -> i32
  emitrust.assign %acc = %t : !emitrust.lvalue<i32>
  emitrust.loop {
    %r0 = emitrust.load %acc : (!emitrust.lvalue<i32>) -> i32
    %n = emitrust.call_opaque "helper"(%r0) : (i32) -> i32
    emitrust.assign %acc = %n : !emitrust.lvalue<i32>
    emitrust.break
  }
  %r = emitrust.load %acc : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// A may-drop binding is NOT barred outright: the gate is about what the GAP
// declares. Here the gap declares only an `i32`, which owns no drop glue, so
// sinking past it is unobservable and the merge stands.
// CHECK-LABEL: fn drop_pure_gap(v0: i32, v1: L) {
// CHECK-NEXT:    let v2: i32 = compute(v0);
// CHECK-NEXT:    println!("{}", v2);
// CHECK-NEXT:    let a: L = v1;
// CHECK-NEXT:    sink(a);
// CHECK-NEXT:  }
emitrust.func @drop_pure_gap(%arg0: i32, %arg1: !emitrust.struct<"L">) {
  %a = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.struct<"L">>
  %c = emitrust.call_opaque "compute"(%arg0) : (i32) -> i32
  emitrust.call_opaque "println!"(%c) {args = ["{}", 0 : index]} : (i32) -> ()
  emitrust.assign %a = %arg1 : !emitrust.lvalue<!emitrust.struct<"L">>
  %ra = emitrust.load %a : (!emitrust.lvalue<!emitrust.struct<"L">>) -> !emitrust.struct<"L">
  emitrust.call_opaque "sink"(%ra) : (!emitrust.struct<"L">) -> ()
  emitrust.return
}

// ---------------------------------------------------------------------------
// The refusals. Each of these keeps today's two-program-point rendering, and
// that is the invariant being pinned -- not the spelling of the win.
// ---------------------------------------------------------------------------

// REFUSE: every write is inside the `if`'s arms. Sinking the declaration into
// an arm would put the binding out of scope for the read after the `if`.
// (This is control-flow.mlir's `if_stmt_mut` shape: two writes in one arm
// defeat FR-61b's if-expression binding, so the statement form is all there
// is.)
// CHECK-LABEL: fn region_write_if(v0: bool, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    let mut v3: i32;
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:      v3 = v1;
// CHECK-NEXT:      v3 = v2;
// CHECK-NEXT:    } else {
// CHECK-NEXT:      v3 = v2;
// CHECK-NEXT:    }
// CHECK-NEXT:    v3
// CHECK-NEXT:  }
emitrust.func @region_write_if(%arg0: i1, %arg1: i32, %arg2: i32) -> i32 {
  %0 = emitrust.let mut %arg1 : i32
  emitrust.if %arg0 {
    emitrust.assign %0 = %arg1 : i32
    emitrust.assign %0 = %arg2 : i32
  } else {
    emitrust.assign %0 = %arg2 : i32
  }
  emitrust.return %0 : i32
}

// REFUSE: the only write is inside a `loop` body, and the read is after the
// loop -- the SCF-destruction shape. Sinking would both change scope and, for
// a loop, re-declare on every iteration.
// CHECK-LABEL: fn region_write_loop(v0: i32, v1: bool) -> i32 {
// CHECK-NEXT:    let mut acc: i32;
// CHECK-NEXT:    loop {
// CHECK-NEXT:      acc = v0;
emitrust.func @region_write_loop(%arg0: i32, %arg1: i1) -> i32 {
  %acc = emitrust.variable named "acc" : !emitrust.lvalue<i32>
  emitrust.loop {
    emitrust.assign %acc = %arg0 : !emitrust.lvalue<i32>
    emitrust.if %arg1 {
      emitrust.break
    }
  }
  %r = emitrust.load %acc : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// REFUSE: a projection of the binding (`p.x`) sits in the gap. Its store is
// dead and renders nothing, but the scan refuses on ANY mention of the binding
// before its whole-and-direct write rather than reasoning about which mentions
// render -- the whole/direct test is `assign.getVar() == binding`, so a
// projection write is never mistaken for the initializing write.
// CHECK-LABEL: fn gap_mentions_binding(v0: i32) -> i32 {
// CHECK-NEXT:    let p: P;
// CHECK-NEXT:    let v1: P = mkp(v0);
// CHECK-NEXT:    p = v1;
// CHECK-NEXT:    p.x
// CHECK-NEXT:  }
emitrust.func @gap_mentions_binding(%arg0: i32) -> i32 {
  %v = emitrust.variable named "p" : !emitrust.lvalue<!emitrust.struct<"P">>
  %m = emitrust.member %v["x"] : (!emitrust.lvalue<!emitrust.struct<"P">>) -> !emitrust.lvalue<i32>
  emitrust.assign %m = %arg0 : !emitrust.lvalue<i32>
  %w = emitrust.call_opaque "mkp"(%arg0) : (i32) -> !emitrust.struct<"P">
  emitrust.assign %v = %w : !emitrust.lvalue<!emitrust.struct<"P">>
  %r = emitrust.member %v["x"] : (!emitrust.lvalue<!emitrust.struct<"P">>) -> !emitrust.lvalue<i32>
  %l = emitrust.load %r : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %l : i32
}

// FR-61b KEEPS PRIORITY: an if-expression binding already folds both program
// points, and into a strictly better rendering. The new fold must not preempt
// it (it cannot anyway -- the writes are inside regions -- but the hook order
// is what guarantees it).
// CHECK-LABEL: fn if_expr_priority(v0: bool, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    let v3: i32 = if v0 {
// CHECK-NEXT:      v1
// CHECK-NEXT:    } else {
// CHECK-NEXT:      v2
// CHECK-NEXT:    };
// CHECK-NEXT:    v3 + v3
// CHECK-NEXT:  }
emitrust.func @if_expr_priority(%arg0: i1, %arg1: i32, %arg2: i32) -> i32 {
  %0 = emitrust.let mut %arg1 : i32
  emitrust.if %arg0 {
    emitrust.assign %0 = %arg1 : i32
  } else {
    emitrust.assign %0 = %arg2 : i32
  }
  %r = emitrust.add %0, %0 : i32
  emitrust.return %r : i32
}

// DROP ORDER -- the soundness pin. `a` and `b` are both `has_drop` structs,
// both still owning at scope end (`sink` takes them by value only after both
// are live... in the emitted Rust they are moved, but the gate must not depend
// on that). Merging `a` would emit `b`'s declaration first and flip the two
// destructors. Both orderings compile clean, so only this pin catches it.
// CHECK-LABEL: fn drop_order(v0: i32) {
// CHECK-NEXT:    let a: L;
// CHECK-NEXT:    let b: L;
// CHECK-NEXT:    let v1: L = mk(v0);
// CHECK-NEXT:    b = v1;
// CHECK-NEXT:    let v2: L = mk(v0);
// CHECK-NEXT:    a = v2;
// CHECK-NEXT:    sink(a, b);
// CHECK-NEXT:  }
emitrust.func @drop_order(%arg0: i32) {
  %a = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.struct<"L">>
  %b = emitrust.variable named "b" : !emitrust.lvalue<!emitrust.struct<"L">>
  %mb = emitrust.call_opaque "mk"(%arg0) : (i32) -> !emitrust.struct<"L">
  emitrust.assign %b = %mb : !emitrust.lvalue<!emitrust.struct<"L">>
  %ma = emitrust.call_opaque "mk"(%arg0) : (i32) -> !emitrust.struct<"L">
  emitrust.assign %a = %ma : !emitrust.lvalue<!emitrust.struct<"L">>
  %ra = emitrust.load %a : (!emitrust.lvalue<!emitrust.struct<"L">>) -> !emitrust.struct<"L">
  %rb = emitrust.load %b : (!emitrust.lvalue<!emitrust.struct<"L">>) -> !emitrust.struct<"L">
  emitrust.call_opaque "sink"(%ra, %rb) : (!emitrust.struct<"L">, !emitrust.struct<"L">) -> ()
  emitrust.return
}

// The twin of @drop_order with `i32` in place of `L` -- byte-for-byte the same
// op shape. It merges, which proves the refusal above is the DROP GATE and not
// some incidental property of the shape.
// CHECK-LABEL: fn drop_order_scalar_twin(v0: i32) {
// CHECK-NEXT:    let v1: i32 = mk(v0);
// CHECK-NEXT:    let b: i32 = v1;
// CHECK-NEXT:    let v2: i32 = mk(v0);
// CHECK-NEXT:    let a: i32 = v2;
// CHECK-NEXT:    sink(a, b);
// CHECK-NEXT:  }
emitrust.func @drop_order_scalar_twin(%arg0: i32) {
  %a = emitrust.variable named "a" : !emitrust.lvalue<i32>
  %b = emitrust.variable named "b" : !emitrust.lvalue<i32>
  %mb = emitrust.call_opaque "mk"(%arg0) : (i32) -> i32
  emitrust.assign %b = %mb : !emitrust.lvalue<i32>
  %ma = emitrust.call_opaque "mk"(%arg0) : (i32) -> i32
  emitrust.assign %a = %ma : !emitrust.lvalue<i32>
  %ra = emitrust.load %a : (!emitrust.lvalue<i32>) -> i32
  %rb = emitrust.load %b : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "sink"(%ra, %rb) : (i32, i32) -> ()
  emitrust.return
}

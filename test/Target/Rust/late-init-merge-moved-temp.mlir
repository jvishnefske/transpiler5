// FR-132's DROP-ORDER GATE, NARROWED -- and the narrowing's fence pinned on
// hand-written IR, which is the only place the dangerous shape is reachable
// (both C++ routes to it are located rejections, per FR-132).
//
// THE GATE, unchanged in intent: Rust drops in reverse DECLARATION order, so
// merging `let x: T; .. x = v;` into `let x: T = v;` SINKS x's declaration
// past everything in the gap, and if the gap declares another value that
// still owns at scope end the two destructors swap. Both orderings compile
// clean, so only a golden like this one can see it.
//
// THE NARROWING, and the whole of its proof: a gap value whose ONLY use is
// the merging assignment itself has no destructor left to reorder.
//   * If its type is `Copy`, Rust forbids it from also being `Drop` -- there
//     is no destructor at all. (`typeMayDrop` is deliberately WIDER than
//     `has_drop`: it answers true for every opaque owner and every data enum,
//     including `Copy` ones like `Option<i32>`.)
//   * If its type is non-`Copy`, the assignment is a MOVE, so its scope-end
//     drop is already a no-op -- in the merged spelling exactly as in the
//     unmerged one.
// Both readings land on the same answer, which is why this needs no move
// analysis, no liveness, and no reasoning about what the gap does.
//
// EVERY OTHER GAP VALUE STILL REFUSES. The refusal legs below are the fence
// and each one is a value the narrowing must NOT admit: a second live
// binding (@drop_order in the sibling golden), a CHAIN whose first link feeds
// the second rather than the assign, a gap value read AFTER the merge point,
// and a gap value used twice. Widening any of them needs a move analysis this
// fold does not have.
//
// The declaration's name is claimed even when it renders nothing, so
// v-numbering is unchanged. A shifted vNN is a bug, not churn.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

emitrust.struct_def @L ["id"] [i32] {emitrust.has_drop}

// ---------------------------------------------------------------------------
// The win.
// ---------------------------------------------------------------------------

// THE MOTIVATING SHAPE: the gap holds exactly one may-drop value, and it is
// the value the assignment consumes. Byte-for-byte the same op shape as
// @drop_order in late-init-merge.mlir minus the SECOND binding -- so this
// pair proves the gate now discriminates on the gap value's USE, not merely
// on its type.
// CHECK-LABEL: fn moved_temp(v0: i32) {
// CHECK-NEXT:    let v1: L = mk(v0);
// CHECK-NEXT:    let a: L = v1;
// CHECK-NEXT:    sink(a);
// CHECK-NEXT:  }
emitrust.func @moved_temp(%arg0: i32) {
  %a = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.struct<"L">>
  %ma = emitrust.call_opaque "mk"(%arg0) : (i32) -> !emitrust.struct<"L">
  emitrust.assign %a = %ma : !emitrust.lvalue<!emitrust.struct<"L">>
  %ra = emitrust.load %a : (!emitrust.lvalue<!emitrust.struct<"L">>) -> !emitrust.struct<"L">
  emitrust.call_opaque "sink"(%ra) : (!emitrust.struct<"L">) -> ()
  emitrust.return
}

// The gap keeps a statement that renders and cannot observe the binding. The
// declaration still sinks all the way onto the write -- the rule is the gap
// value's USE, not adjacency.
// CHECK-LABEL: fn moved_temp_gapped(v0: i32) {
// CHECK-NEXT:    let v1: L = mk(v0);
// CHECK-NEXT:    println!("i={}", v0);
// CHECK-NEXT:    let a: L = v1;
// CHECK-NEXT:    sink(a);
// CHECK-NEXT:  }
emitrust.func @moved_temp_gapped(%arg0: i32) {
  %a = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.struct<"L">>
  %ma = emitrust.call_opaque "mk"(%arg0) : (i32) -> !emitrust.struct<"L">
  emitrust.call_opaque "println!"(%arg0) {args = ["i={}", 0 : index]} : (i32) -> ()
  emitrust.assign %a = %ma : !emitrust.lvalue<!emitrust.struct<"L">>
  %ra = emitrust.load %a : (!emitrust.lvalue<!emitrust.struct<"L">>) -> !emitrust.struct<"L">
  emitrust.call_opaque "sink"(%ra) : (!emitrust.struct<"L">) -> ()
  emitrust.return
}

// ---------------------------------------------------------------------------
// The fence. Each leg is a gap value the narrowing must NOT admit.
// ---------------------------------------------------------------------------

// REFUSES -- THE CHAIN. `%m1` feeds `%m2`, and only `%m2` feeds the assign.
// `%m1` is very probably moved into `wrap`, but "probably" is not a proof:
// deciding it needs to know whether the emitted callee takes its argument by
// value or by reference, which is exactly the move analysis this fold does
// not have. This is the shape `std::make_unique<Node>` lowers to
// (test/EndToEnd/late-init-moved-temp.cpp's `boxed_dtor`), so the refusal is
// live on real C++ input and not merely defensive.
// CHECK-LABEL: fn chain(v0: i32) {
// CHECK-NEXT:    let a: L;
// CHECK-NEXT:    let v1: L = mk(v0);
// CHECK-NEXT:    let v2: L = wrap(v1);
// CHECK-NEXT:    a = v2;
// CHECK-NEXT:    sink(a);
// CHECK-NEXT:  }
emitrust.func @chain(%arg0: i32) {
  %a = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.struct<"L">>
  %m1 = emitrust.call_opaque "mk"(%arg0) : (i32) -> !emitrust.struct<"L">
  %m2 = emitrust.call_opaque "wrap"(%m1) : (!emitrust.struct<"L">) -> !emitrust.struct<"L">
  emitrust.assign %a = %m2 : !emitrust.lvalue<!emitrust.struct<"L">>
  %ra = emitrust.load %a : (!emitrust.lvalue<!emitrust.struct<"L">>) -> !emitrust.struct<"L">
  emitrust.call_opaque "sink"(%ra) : (!emitrust.struct<"L">) -> ()
  emitrust.return
}

// REFUSES -- USED AFTER THE MERGE POINT. `%live` is declared in the gap and
// consumed only after the write, so at the write it is still owning: sinking
// `a` past it would put `a`'s destructor first. This is the narrowing's
// sharpest failure mode and the reason the rule is "the use IS the merging
// assign" rather than "the value has one use".
// CHECK-LABEL: fn live_across(v0: i32) {
// CHECK-NEXT:    let a: L;
// CHECK-NEXT:    let v1: L = mk(v0);
// CHECK-NEXT:    let v2: L = mk(v0);
// CHECK-NEXT:    a = v2;
// CHECK-NEXT:    sink(a);
// CHECK-NEXT:    sink(v1);
// CHECK-NEXT:  }
emitrust.func @live_across(%arg0: i32) {
  %a = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.struct<"L">>
  %live = emitrust.call_opaque "mk"(%arg0) : (i32) -> !emitrust.struct<"L">
  %ma = emitrust.call_opaque "mk"(%arg0) : (i32) -> !emitrust.struct<"L">
  emitrust.assign %a = %ma : !emitrust.lvalue<!emitrust.struct<"L">>
  %ra = emitrust.load %a : (!emitrust.lvalue<!emitrust.struct<"L">>) -> !emitrust.struct<"L">
  emitrust.call_opaque "sink"(%ra) : (!emitrust.struct<"L">) -> ()
  emitrust.call_opaque "sink"(%live) : (!emitrust.struct<"L">) -> ()
  emitrust.return
}

// REFUSES -- TWO USES. The assign consumes the value, but so does the call
// before it. One of the two renderings is not a move, so nothing is proved.
// CHECK-LABEL: fn two_uses(v0: i32) {
// CHECK-NEXT:    let a: L;
// CHECK-NEXT:    let v1: L = mk(v0);
// CHECK-NEXT:    peek(v1);
// CHECK-NEXT:    a = v1;
// CHECK-NEXT:    sink(a);
// CHECK-NEXT:  }
emitrust.func @two_uses(%arg0: i32) {
  %a = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.struct<"L">>
  %ma = emitrust.call_opaque "mk"(%arg0) : (i32) -> !emitrust.struct<"L">
  emitrust.call_opaque "peek"(%ma) : (!emitrust.struct<"L">) -> ()
  emitrust.assign %a = %ma : !emitrust.lvalue<!emitrust.struct<"L">>
  %ra = emitrust.load %a : (!emitrust.lvalue<!emitrust.struct<"L">>) -> !emitrust.struct<"L">
  emitrust.call_opaque "sink"(%ra) : (!emitrust.struct<"L">) -> ()
  emitrust.return
}

// REFUSES -- A SECOND LIVE BINDING in the gap, the @drop_order shape from the
// sibling golden reduced to its core: `b` is a may-drop DECLARATION, not a
// consumed temporary, and merging `a` would emit `b` first and swap the two
// destructors. Restated here so the narrowing's own golden carries the shape
// it must never admit.
// CHECK-LABEL: fn second_binding(v0: i32) {
// CHECK-NEXT:    let a: L;
// CHECK-NEXT:    let v1: L = mk(v0);
// CHECK-NEXT:    let b: L = v1;
// CHECK-NEXT:    let v2: L = mk(v0);
// CHECK-NEXT:    a = v2;
// CHECK-NEXT:    sink2(a, b);
// CHECK-NEXT:  }
emitrust.func @second_binding(%arg0: i32) {
  %a = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.struct<"L">>
  %b = emitrust.variable named "b" : !emitrust.lvalue<!emitrust.struct<"L">>
  %mb = emitrust.call_opaque "mk"(%arg0) : (i32) -> !emitrust.struct<"L">
  emitrust.assign %b = %mb : !emitrust.lvalue<!emitrust.struct<"L">>
  %ma = emitrust.call_opaque "mk"(%arg0) : (i32) -> !emitrust.struct<"L">
  emitrust.assign %a = %ma : !emitrust.lvalue<!emitrust.struct<"L">>
  %ra = emitrust.load %a : (!emitrust.lvalue<!emitrust.struct<"L">>) -> !emitrust.struct<"L">
  %rb = emitrust.load %b : (!emitrust.lvalue<!emitrust.struct<"L">>) -> !emitrust.struct<"L">
  emitrust.call_opaque "sink2"(%ra, %rb) : (!emitrust.struct<"L">, !emitrust.struct<"L">) -> ()
  emitrust.return
}

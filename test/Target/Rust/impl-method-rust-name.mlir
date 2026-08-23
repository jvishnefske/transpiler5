// FR-110: emitted C++ methods shed their `<Struct>_` mangle at PRINT time
// only. An impl member carrying `emitrust.method_rust_name` renders its
// in-impl spelling after `fn `, its method_call sites render the same
// spelling after `.`, and a qualified static `call_opaque
// "Struct::mangled"` rewrites its right half — while the IR keeps the
// module-unique mangled symbols everywhere (the receiver-mutability
// query, the FR-52 shared symbol namespace, and shard bytecode all read
// the mangled names; a strip baked into the IR would collide two classes'
// `get`). Pins the whole contract in one golden:
//  * two impls each owning a member stripped to `get` (cross-impl
//    no-collision, one `&self`, one `&mut self`) — and the receiver
//    mutability query still keys on the UNSTRIPPED member symbol, so the
//    `&mut` receiver's binding renders `let mut` while the `&self` one
//    stays immutable;
//  * a static member stripped inside a qualified call_opaque
//    (`BoxA::origin()`), while call_opaque strings whose right half is
//    not a mapped member symbol (`BoxA::default`) or whose left half is
//    not the member's own impl (`Other::box_a_origin`) pass through
//    verbatim;
//  * an attr-less impl member (a Phase-4 C owner method, the actor-lift
//    shape, whose symbol was never struct-prefixed) keeps its symbol at
//    the def AND the call site — the strip is keyed by the importer
//    attribute, never by name-prefix parsing (a C impl legitimately holds
//    both `counter_actor_get` and `get` today; a blind prefix strip
//    collides them, measured in the FR-110 spike).
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK: impl BoxA {
emitrust.impl "BoxA" {
  // CHECK: fn get(&self) -> i32 {
  emitrust.func @box_a_get(%arg0: !emitrust.ref<!emitrust.struct<"BoxA">>) -> i32 attributes {emitrust.method_rust_name = "get"} {
    %s = emitrust.deref %arg0 : (!emitrust.ref<!emitrust.struct<"BoxA">>) -> !emitrust.lvalue<!emitrust.struct<"BoxA">>
    %m = emitrust.member %s["value"] : (!emitrust.lvalue<!emitrust.struct<"BoxA">>) -> !emitrust.lvalue<i32>
    %v = emitrust.load %m : (!emitrust.lvalue<i32>) -> i32
    emitrust.return %v : i32
  }
  // CHECK: fn origin() -> i32 {
  emitrust.func @box_a_origin() -> i32 attributes {emitrust.method_rust_name = "origin", emitrust.static_method} {
    %c = emitrust.constant <0 : i32> : i32
    emitrust.return %c : i32
  }
// CHECK: }
}

// CHECK: impl BoxB {
emitrust.impl "BoxB" {
  // The SAME in-impl spelling `get` in a second impl: module symbols stay
  // distinct (box_a_get / box_b_get), only the print collapses.
  // CHECK: fn get(&mut self) -> i32 {
  emitrust.func @box_b_get(%arg0: !emitrust.mut_ref<!emitrust.struct<"BoxB">>) -> i32 attributes {emitrust.method_rust_name = "get"} {
    %c = emitrust.constant <7 : i32> : i32
    emitrust.return %c : i32
  }
// CHECK: }
}

// CHECK: impl Owner {
emitrust.impl "Owner" {
  // No attribute: a Phase-4 C owner method keeps its own symbol verbatim.
  // CHECK: fn bump(&mut self) {
  emitrust.func @bump(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner">>) {
    emitrust.return
  }
// CHECK: }
}

// CHECK: fn c_main() -> i32 {
emitrust.func @c_main() -> i32 {
  // The &self receiver stays immutable, the &mut one takes `let mut` —
  // both resolved through the UNSTRIPPED member symbols in the IR below.
  // CHECK: let v0: BoxA = BoxA::default();
  %a = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"BoxA">>
  // CHECK: let mut v1: BoxB = BoxB::default();
  %b = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"BoxB">>
  // CHECK: let mut v2: Owner = Owner::default();
  %o = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Owner">>
  // CHECK: v0.get()
  %va = emitrust.method_call %a["box_a_get"] () : (!emitrust.lvalue<!emitrust.struct<"BoxA">>) -> i32
  // CHECK: v1.get()
  %vb = emitrust.method_call %b["box_b_get"] () : (!emitrust.lvalue<!emitrust.struct<"BoxB">>) -> i32
  // CHECK: v2.bump();
  emitrust.method_call %o["bump"] () : (!emitrust.lvalue<!emitrust.struct<"Owner">>) -> ()
  // The IR carries the importer's mangled-on-both-sides qualified string
  // (the methods.cpp RED-pinned scheme); the emitter rewrites ONLY the
  // right half, and only because `box_a_origin` maps inside impl BoxA.
  // CHECK: BoxA::origin()
  // CHECK-NOT: BoxA::box_a_origin
  %vo = emitrust.call_opaque "BoxA::box_a_origin"() : () -> i32
  // Negative controls: an unmapped right half and a foreign left half
  // pass through verbatim.
  // CHECK: BoxA::default()
  %vd = emitrust.call_opaque "BoxA::default"() : () -> !emitrust.struct<"BoxA">
  // CHECK: Other::box_a_origin()
  %vx = emitrust.call_opaque "Other::box_a_origin"() : () -> i32
  %s0 = emitrust.add %va, %vb : i32
  %s1 = emitrust.add %s0, %vo : i32
  %s2 = emitrust.add %s1, %vx : i32
  emitrust.return %s2 : i32
}

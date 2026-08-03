// FR-62 slice 5b: pins the THREADED rewrite of emitrust-actor-thread on
// the post-lift shape — the invariant that a selected actor's driver
// accesses become synthesized accessor METHODS (ordinary impl funcs with
// the spike's deref/member/subscript bodies and param_names slots, in
// struct-field order, get before set), the driver rewrite is
// order-preserving (each load -> get_ call at the load's site, each
// assign -> set_ call at the assign's site, scalar and array-element
// forms), the actor local retypes to the opaque <Actor>Handle constructed
// by `<Actor>Handle::spawn(<Actor>::default())` around the original
// variable, `.shutdown()` lands before the driver return, one
// `emitrust.actor_runtime` anchor follows the impl, and the consumed
// `emitrust.actor_thread` attribute is STRIPPED (--implicit-check-not).
// The output must re-verify, so the anchor's own sendable checks run
// against the synthesized accessors here too.
// RUN: emitrust-opt %s --emitrust-actor-thread \
// RUN:   | FileCheck %s --implicit-check-not='emitrust.actor_thread'

module attributes {
  emitrust.actor_thread = [{name = "CounterActor", mode = "threaded"}]} {
  emitrust.struct_def @CounterActor ["counter", "table"] [i32, !emitrust.array<4xi32>]
  // CHECK:      emitrust.impl "CounterActor" {
  // CHECK:        emitrust.func @bump
  emitrust.impl "CounterActor" {
    emitrust.func @bump(%arg0: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> i32 {
      %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<!emitrust.struct<"CounterActor">>
      %1 = emitrust.member %0["counter"] : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<i32>
      %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
      %3 = emitrust.constant <1 : i32> : i32
      %4 = emitrust.add %2, %3 : i32
      emitrust.assign %1 = %4 : !emitrust.lvalue<i32>
      %5 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
      emitrust.return %5 : i32
    }
  }

  // The synthesized accessors: struct-field order (counter before table),
  // get before set; scalar forms take no index, element forms take the
  // subscript's index type with param_names ["", "i"(, "v")].
  // CHECK:        emitrust.func @get_counter(%[[SELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> i32 {
  // CHECK-NEXT:     %[[P:.*]] = emitrust.deref %[[SELF]]
  // CHECK-NEXT:     %[[M:.*]] = emitrust.member %[[P]]["counter"]
  // CHECK-NEXT:     %[[V:.*]] = emitrust.load %[[M]]
  // CHECK-NEXT:     emitrust.return %[[V]] : i32
  // CHECK-NEXT:   }
  // CHECK-NEXT:   emitrust.func @set_counter(%[[SELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>, %[[ARG:.*]]: i32) attributes {emitrust.param_names = ["", "v"]} {
  // CHECK-NEXT:     %[[P:.*]] = emitrust.deref %[[SELF]]
  // CHECK-NEXT:     %[[M:.*]] = emitrust.member %[[P]]["counter"]
  // CHECK-NEXT:     emitrust.assign %[[M]] = %[[ARG]] : !emitrust.lvalue<i32>
  // CHECK-NEXT:     emitrust.return
  // CHECK-NEXT:   }
  // CHECK-NEXT:   emitrust.func @get_table(%[[SELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>, %[[I:.*]]: i32) -> i32 attributes {emitrust.param_names = ["", "i"]} {
  // CHECK-NEXT:     %[[P:.*]] = emitrust.deref %[[SELF]]
  // CHECK-NEXT:     %[[M:.*]] = emitrust.member %[[P]]["table"]
  // CHECK-NEXT:     %[[S:.*]] = emitrust.subscript %[[M]][%[[I]]]
  // CHECK-NEXT:     %[[V:.*]] = emitrust.load %[[S]]
  // CHECK-NEXT:     emitrust.return %[[V]] : i32
  // CHECK-NEXT:   }
  // CHECK-NEXT:   emitrust.func @set_table(%[[SELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>, %[[I:.*]]: i32, %[[ARG:.*]]: i32) attributes {emitrust.param_names = ["", "i", "v"]} {
  // CHECK-NEXT:     %[[P:.*]] = emitrust.deref %[[SELF]]
  // CHECK-NEXT:     %[[M:.*]] = emitrust.member %[[P]]["table"]
  // CHECK-NEXT:     %[[S:.*]] = emitrust.subscript %[[M]][%[[I]]]
  // CHECK-NEXT:     emitrust.assign %[[S]] = %[[ARG]] : !emitrust.lvalue<i32>
  // CHECK-NEXT:     emitrust.return
  // CHECK-NEXT:   }
  // CHECK-NEXT: }

  // The anchor, after the impl, carrying the mode.
  // CHECK-NEXT: emitrust.actor_runtime @CounterActor mode = threaded

  // The driver: spawn construction around the retyped local, accesses as
  // handle method calls at the original sites, shutdown before return.
  // CHECK:      emitrust.func @c_main() -> i32 {
  // CHECK-NEXT:   %[[ST:.*]] = emitrust.call_opaque "CounterActor::default"() : () -> !emitrust.struct<"CounterActor">
  // CHECK-NEXT:   %[[H:.*]] = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>
  // CHECK-NEXT:   %[[SP:.*]] = emitrust.call_opaque "CounterActorHandle::spawn"(%[[ST]]) : (!emitrust.struct<"CounterActor">) -> !emitrust.opaque<"CounterActorHandle">
  // CHECK-NEXT:   emitrust.assign %[[H]] = %[[SP]] : !emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>
  emitrust.func @c_main() -> i32 {
    %0 = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.struct<"CounterActor">>
    %1 = emitrust.member %0["counter"] : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<i32>
    %2 = emitrust.member %0["table"] : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<!emitrust.array<4xi32>>
    %3 = emitrust.constant <1 : i32> : i32
    %4 = emitrust.constant <2 : i32> : i32
    // CHECK-NEXT:   %[[K1:.*]] = emitrust.constant <1 : i32> : i32
    // CHECK-NEXT:   %[[K2:.*]] = emitrust.constant <2 : i32> : i32
    // CHECK-NEXT:   %[[G:.*]] = emitrust.method_call %[[H]]["get_counter"] () : (!emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>) -> i32
    // CHECK-NEXT:   emitrust.method_call %[[H]]["set_counter"] (%[[G]]) : (!emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>, i32) -> ()
    // CHECK-NEXT:   %[[E:.*]] = emitrust.method_call %[[H]]["get_table"] (%[[K2]]) : (!emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>, i32) -> i32
    // CHECK-NEXT:   emitrust.method_call %[[H]]["set_table"] (%[[K1]], %[[E]]) : (!emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>, i32, i32) -> ()
    // CHECK-NEXT:   %[[B:.*]] = emitrust.method_call %[[H]]["bump"] () : (!emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>) -> i32
    // CHECK-NEXT:   emitrust.method_call %[[H]]["shutdown"] () : (!emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>) -> ()
    // CHECK-NEXT:   emitrust.return %[[B]] : i32
    %5 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
    emitrust.assign %1 = %5 : !emitrust.lvalue<i32>
    %6 = emitrust.subscript %2[%4] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
    %7 = emitrust.load %6 : (!emitrust.lvalue<i32>) -> i32
    %8 = emitrust.subscript %2[%3] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
    emitrust.assign %8 = %7 : !emitrust.lvalue<i32>
    %9 = emitrust.method_call %0["bump"] () : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> i32
    emitrust.return %9 : i32
  }
}

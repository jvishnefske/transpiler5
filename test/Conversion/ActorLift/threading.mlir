// FR-62 slice 4 (stage A): pins the THREADING shape — the SLICE-4 SPIKE's
// resolved question of how an actor reference reaches an arm that touches
// no globals itself but sits on the call path to one that does. The
// invariant: a helper the plan roles as arm-by-closure lifts INTO the impl,
// its calls to sibling arms become method_calls on the dereferenced
// receiver (rendering `(*self).bump()`), and the driver calls the helper as
// an ordinary method on the actor local. This is the hand-lifted
// s4-threading.actor.mlir reference shape.
// RUN: emitrust-opt %s --emitrust-actor-lift \
// RUN:   | FileCheck %s --implicit-check-not='emitrust.global @'

// CHECK: emitrust.struct_def @CounterActor ["counter"] [i32]
module attributes {
  emitrust.actor_lift = [{name = "CounterActor", var = "counter_actor",
                          globals = ["COUNTER"], fields = ["counter"]}]} {
  emitrust.global @COUNTER : i32

  // CHECK:      emitrust.impl "CounterActor" {
  // CHECK:        emitrust.func @bump(%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> i32
  emitrust.func @bump() -> i32 attributes {emitrust.actor_arm = "CounterActor"} {
    %0 = emitrust.constant <2 : i32> : i32
    %1 = emitrust.global_load @COUNTER : i32
    %2 = emitrust.add %1, %0 : i32
    emitrust.global_store %2, @COUNTER : i32
    %3 = emitrust.global_load @COUNTER : i32
    emitrust.return %3 : i32
  }

  // The helper: receiver prepended (its param_names gains the empty
  // receiver slot ahead of "k"), and both bump() calls become method_calls
  // on the SAME dereferenced receiver.
  // CHECK:        emitrust.func @helper(%[[SELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>, %[[K:.*]]: i32) -> i32
  // CHECK-SAME:     emitrust.param_names = ["", "k"]
  // CHECK:          %[[P:.*]] = emitrust.deref %[[SELF]]
  // CHECK:          %[[A:.*]] = emitrust.method_call %[[P]]["bump"] () : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> i32
  // CHECK:          %[[B:.*]] = emitrust.method_call %[[P]]["bump"] () : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> i32
  // CHECK:          %[[S:.*]] = emitrust.add %[[A]], %[[B]] : i32
  // CHECK:          emitrust.add %[[S]], %[[K]] : i32
  emitrust.func @helper(%arg0: i32) -> i32
      attributes {emitrust.actor_arm = "CounterActor",
                  emitrust.param_names = ["k"]} {
    %0 = emitrust.call_opaque "bump"() : () -> i32
    %1 = emitrust.call_opaque "bump"() : () -> i32
    %2 = emitrust.add %0, %1 : i32
    %3 = emitrust.add %2, %arg0 : i32
    emitrust.return %3 : i32
  }
  // CHECK:      }

  // CHECK:      emitrust.func @c_main() -> i32 {
  // CHECK:        %[[CA:.*]] = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.struct<"CounterActor">>
  // CHECK:        emitrust.method_call %[[CA]]["helper"] (%{{.*}}) : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>, i32) -> i32
  // CHECK-NOT:    emitrust.call_opaque "helper"
  emitrust.func @c_main() -> i32 attributes {emitrust.actor_driver} {
    %0 = emitrust.constant <1 : i32> : i32
    %1 = emitrust.call_opaque "helper"(%0) : (i32) -> i32
    emitrust.return %1 : i32
  }
}

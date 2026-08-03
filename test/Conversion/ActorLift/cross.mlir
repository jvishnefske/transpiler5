// FR-62 slice 4 (stage A): pins the CROSS shape — the SLICE-4 SPIKE's
// two-actor client mechanics. The invariants: a cross-actor client stays at
// module level and gains one !emitrust.mut_ref parameter per
// closure-footprint actor in the attribute's (driver-sorted) order, with
// param_names carrying the actor variable names; its calls to arms become
// method_calls on the matching dereferenced parameter; a recursive or
// cross-to-cross call passes the caller's own mut_ref parameters straight
// through (Rust's implicit reborrow, no re-borrow ops); and the driver
// materializes a FRESH `emitrust.addr_of mut` per cross call site while
// calling single-actor arms as plain methods. This is the hand-lifted
// s4-cross2.actor.mlir reference shape.
// RUN: emitrust-opt %s --emitrust-actor-lift \
// RUN:   | FileCheck %s --implicit-check-not='emitrust.global @'

// CHECK-DAG: emitrust.struct_def @LeftActor ["left"] [i32]
// CHECK-DAG: emitrust.struct_def @RightActor ["right"] [i32]
module attributes {
  emitrust.actor_lift = [{name = "LeftActor", var = "left_actor",
                          globals = ["LEFT"], fields = ["left"]},
                         {name = "RightActor", var = "right_actor",
                          globals = ["RIGHT"], fields = ["right"]}]} {
  emitrust.global @LEFT : i32
  emitrust.global @RIGHT : i32

  emitrust.func @bump_left() -> i32
      attributes {emitrust.actor_arm = "LeftActor"} {
    %0 = emitrust.constant <1 : i32> : i32
    %1 = emitrust.global_load @LEFT : i32
    %2 = emitrust.add %1, %0 : i32
    emitrust.global_store %2, @LEFT : i32
    emitrust.return %2 : i32
  }
  emitrust.func @get_right() -> i32
      attributes {emitrust.actor_arm = "RightActor"} {
    %0 = emitrust.global_load @RIGHT : i32
    emitrust.return %0 : i32
  }

  // The cross client: two mut_ref params in attribute order, named after
  // the actor variables; one method_call per arm on the matching receiver.
  // CHECK:      emitrust.func @poke(%[[L:.*]]: !emitrust.mut_ref<!emitrust.struct<"LeftActor">>, %[[R:.*]]: !emitrust.mut_ref<!emitrust.struct<"RightActor">>, %[[K:.*]]: i32) -> i32
  // CHECK-SAME:   emitrust.param_names = ["left_actor", "right_actor", "k"]
  // CHECK-DAG:    %[[LP:.*]] = emitrust.deref %[[L]]
  // CHECK-DAG:    %[[RP:.*]] = emitrust.deref %[[R]]
  // CHECK:        %[[A:.*]] = emitrust.method_call %[[LP]]["bump_left"] ()
  // CHECK:        %[[B:.*]] = emitrust.method_call %[[RP]]["get_right"] ()
  emitrust.func @poke(%arg0: i32) -> i32
      attributes {emitrust.actor_cross = ["LeftActor", "RightActor"],
                  emitrust.param_names = ["k"]} {
    %0 = emitrust.call_opaque "bump_left"() : () -> i32
    %1 = emitrust.call_opaque "get_right"() : () -> i32
    %2 = emitrust.add %0, %1 : i32
    %3 = emitrust.add %2, %arg0 : i32
    emitrust.return %3 : i32
  }

  // The recursive cross client: the cross-to-cross call to poke and the
  // self-call both pass the caller's own parameters through — no deref, no
  // addr_of is synthesized here.
  // CHECK:      emitrust.func @robserve(%[[L2:.*]]: !emitrust.mut_ref<!emitrust.struct<"LeftActor">>, %[[R2:.*]]: !emitrust.mut_ref<!emitrust.struct<"RightActor">>, %[[N:.*]]: i32) -> i32
  // CHECK-SAME:   emitrust.param_names = ["left_actor", "right_actor", "n"]
  // CHECK-NOT:      emitrust.deref
  // CHECK:          emitrust.call_opaque "poke"(%[[L2]], %[[R2]], %[[N]])
  // CHECK-NOT:      emitrust.addr_of
  // CHECK:          emitrust.call_opaque "robserve"(%[[L2]], %[[R2]], %{{.*}})
  emitrust.func @robserve(%arg0: i32) -> i32
      attributes {emitrust.actor_cross = ["LeftActor", "RightActor"],
                  emitrust.param_names = ["n"]} {
    %0 = emitrust.constant <1 : i32> : i32
    %1 = emitrust.call_opaque "poke"(%arg0) : (i32) -> i32
    %2 = emitrust.sub %arg0, %0 : i32
    %3 = emitrust.call_opaque "robserve"(%2) : (i32) -> i32
    %4 = emitrust.add %1, %3 : i32
    emitrust.return %4 : i32
  }

  // The driver: one fresh &mut PAIR per cross call site (two calls, four
  // addr_of ops), and the single-actor arm called as a method.
  // CHECK:      emitrust.func @c_main() -> i32 {
  // CHECK-DAG:    %[[LA:.*]] = emitrust.variable named "left_actor" : !emitrust.lvalue<!emitrust.struct<"LeftActor">>
  // CHECK-DAG:    %[[RA:.*]] = emitrust.variable named "right_actor" : !emitrust.lvalue<!emitrust.struct<"RightActor">>
  // CHECK:        %[[LR1:.*]] = emitrust.addr_of mut %[[LA]]
  // CHECK:        %[[RR1:.*]] = emitrust.addr_of mut %[[RA]]
  // CHECK:        emitrust.call_opaque "poke"(%[[LR1]], %[[RR1]], %{{.*}})
  // CHECK:        %[[LR2:.*]] = emitrust.addr_of mut %[[LA]]
  // CHECK:        %[[RR2:.*]] = emitrust.addr_of mut %[[RA]]
  // CHECK:        emitrust.call_opaque "robserve"(%[[LR2]], %[[RR2]], %{{.*}})
  // CHECK:        emitrust.method_call %[[LA]]["bump_left"] ()
  emitrust.func @c_main() -> i32 attributes {emitrust.actor_driver} {
    %0 = emitrust.constant <5 : i32> : i32
    %1 = emitrust.call_opaque "poke"(%0) : (i32) -> i32
    %2 = emitrust.call_opaque "robserve"(%1) : (i32) -> i32
    %3 = emitrust.call_opaque "bump_left"() : () -> i32
    %4 = emitrust.add %2, %3 : i32
    emitrust.return %4 : i32
  }
}

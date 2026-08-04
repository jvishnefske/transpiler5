// FR-62 F2 (owner-handle export): pins the EXPORT shape of
// emitrust-actor-lift — the invariant that an actor dict carrying the
// `export` unit key (a library unit: nothing constructs the actors) still
// lifts, but instead of driver construction the pass (a) marks the
// synthesized struct_def with `emitrust.private_fields` so the emitter
// renders a pub struct whose FIELDS stay private, and (b) synthesizes an
// associated `fn new()` (`emitrust.static_method`, no receiver) as the
// impl's FIRST function, carrying the C initializers as the slice-4
// Default-plus-member-assign materialization relocated from the driver
// into the constructor: a scalar init through `emitrust.constant`, an
// aggregate init through a const staging variable loaded once and
// assigned whole, and a no-init actor constructing as a bare default.
// Arms still move behind the &mut-self receiver, a cross client still
// gains one &mut parameter per listed actor, and every lifted global is
// DELETED (--implicit-check-not pins that no mutable global survives).
// RUN: emitrust-opt %s --emitrust-actor-lift \
// RUN:   | FileCheck %s --implicit-check-not='emitrust.global @'

// The exported owner: struct_def carries the private-fields marker.
// CHECK:      emitrust.struct_def @CounterActor ["counter", "table"] [i32, !emitrust.array<2xi32>] {emitrust.private_fields}
// CHECK:      emitrust.impl "CounterActor" {

// new() first: Default construction (a named variable with no init),
// scalar member assign for COUNTER's <5>, staged whole-array assign for
// TABLE's aggregate init, then the owner returned by value.
// CHECK:        emitrust.func @new() -> !emitrust.struct<"CounterActor"> attributes {emitrust.static_method} {
// CHECK-NEXT:     %[[OWN:.*]] = emitrust.variable named "owner" : !emitrust.lvalue<!emitrust.struct<"CounterActor">>
// CHECK-NEXT:     %[[MC:.*]] = emitrust.member %[[OWN]]["counter"]
// CHECK-NEXT:     %[[C5:.*]] = emitrust.constant <5 : i32> : i32
// CHECK-NEXT:     emitrust.assign %[[MC]] = %[[C5]]
// CHECK-NEXT:     %[[MT:.*]] = emitrust.member %[[OWN]]["table"]
// CHECK-NEXT:     %[[STG:.*]] = emitrust.variable const <[1 : i32, 2 : i32]> : !emitrust.lvalue<!emitrust.array<2xi32>>
// CHECK-NEXT:     %[[TV:.*]] = emitrust.load %[[STG]]
// CHECK-NEXT:     emitrust.assign %[[MT]] = %[[TV]]
// CHECK-NEXT:     %[[RET:.*]] = emitrust.load %[[OWN]]
// CHECK-NEXT:     emitrust.return %[[RET]] : !emitrust.struct<"CounterActor">
// CHECK-NEXT:   }

// The arm, moved in after new(): receiver prepended, member rewrite,
// attribute consumed.
// CHECK:        emitrust.func @bump(%[[SELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> i32 {
// CHECK-NOT:      emitrust.actor_arm
// CHECK:          %[[P:.*]] = emitrust.deref %[[SELF]]
// CHECK:          %[[CNT:.*]] = emitrust.member %[[P]]["counter"]
// CHECK-NOT:      emitrust.global_load
// CHECK-NOT:      emitrust.global_store
// CHECK:      }

// The no-init owner: new() is Default construction and the return, nothing
// else.
// CHECK:      emitrust.struct_def @TotalActor ["total"] [i32] {emitrust.private_fields}
// CHECK:      emitrust.impl "TotalActor" {
// CHECK:        emitrust.func @new() -> !emitrust.struct<"TotalActor"> attributes {emitrust.static_method} {
// CHECK-NEXT:     %[[TOWN:.*]] = emitrust.variable named "owner" : !emitrust.lvalue<!emitrust.struct<"TotalActor">>
// CHECK-NEXT:     %[[TRET:.*]] = emitrust.load %[[TOWN]]
// CHECK-NEXT:     emitrust.return %[[TRET]] : !emitrust.struct<"TotalActor">
// CHECK-NEXT:   }
// CHECK:        emitrust.func @add_total(%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"TotalActor">>, %{{.*}}: i32) -> i32

// The cross client stays at module level with one &mut parameter per
// listed actor and its arm calls rewritten to method calls on the
// dereffed parameters.
// CHECK:      emitrust.func @combined(%[[CA:.*]]: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>, %[[TA:.*]]: !emitrust.mut_ref<!emitrust.struct<"TotalActor">>) -> i32
// CHECK-NOT:    emitrust.actor_cross
// CHECK:        %[[CP:.*]] = emitrust.deref %[[CA]]
// CHECK:        %[[TP:.*]] = emitrust.deref %[[TA]]
// CHECK:        %[[B:.*]] = emitrust.method_call %[[CP]]["bump"] ()
// CHECK:        %[[T:.*]] = emitrust.method_call %[[TP]]["add_total"] (%[[B]])
// CHECK:        emitrust.return %[[T]] : i32

// CHECK-NOT: emitrust.actor_lift
module attributes {
  emitrust.actor_lift = [{name = "CounterActor", var = "counter_actor",
                          globals = ["COUNTER", "TABLE"],
                          fields = ["counter", "table"], export},
                         {name = "TotalActor", var = "total_actor",
                          globals = ["TOTAL"], fields = ["total"],
                          export}]} {
  emitrust.global @COUNTER <5 : i32> : i32
  emitrust.global @TABLE <[1 : i32, 2 : i32]> : !emitrust.array<2xi32>
  emitrust.global @TOTAL : i32

  emitrust.func @bump() -> i32
      attributes {emitrust.actor_arm = "CounterActor"} {
    %0 = emitrust.global_load @COUNTER : i32
    %1 = emitrust.constant <1 : i32> : i32
    %2 = emitrust.add %0, %1 : i32
    emitrust.global_store %2, @COUNTER : i32
    %3 = emitrust.global_load @COUNTER : i32
    emitrust.return %3 : i32
  }

  emitrust.func @add_total(%arg0: i32) -> i32
      attributes {emitrust.actor_arm = "TotalActor",
                  emitrust.param_names = ["x"]} {
    %0 = emitrust.global_load @TOTAL : i32
    %1 = emitrust.add %0, %arg0 : i32
    emitrust.global_store %1, @TOTAL : i32
    %2 = emitrust.global_load @TOTAL : i32
    emitrust.return %2 : i32
  }

  emitrust.func @combined() -> i32
      attributes {emitrust.actor_cross = ["CounterActor", "TotalActor"]} {
    %0 = emitrust.call_opaque "bump"() : () -> i32
    %1 = emitrust.call_opaque "add_total"(%0) : (i32) -> i32
    emitrust.return %1 : i32
  }
}

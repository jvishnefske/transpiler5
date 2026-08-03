// FR-62 slice 4 (stage A): pins the GLOBALS shape of emitrust-actor-lift —
// the invariant that a certified actor's mutable globals become fields of a
// synthesized struct_def, its arms move into an impl behind a &mut-self
// receiver, a const global referenced from an arm crosses the
// impl-SymbolTable wall through the opaque-constant path, the driver
// constructs each actor local (Default + post-construction member assigns
// for non-default initializers), a driver-only global becomes a named
// driver local carrying its initializer, whole-global snapshot round-trips
// elide to direct member access, and every lifted emitrust.global is
// DELETED (no thread_local survives). This is the hand-lifted
// globals.actor.mlir reference shape from the SLICE-4 SPIKE, pinned at the
// pass level with driver-attached attributes standing in for the driver.
// The --implicit-check-not pins the global-deletion invariant everywhere:
// no MUTABLE `emitrust.global @...` survives the lift (the const @SCALE
// stays, spelled `emitrust.global const @`, which the pattern skips).
// RUN: emitrust-opt %s --emitrust-actor-lift \
// RUN:   | FileCheck %s --implicit-check-not='emitrust.global @'

// CHECK:     emitrust.struct_def @CounterActor ["counter", "table"] [i32, !emitrust.array<4xi32>]
module attributes {
  emitrust.actor_lift = [{name = "CounterActor", var = "counter_actor",
                          globals = ["COUNTER", "TABLE"],
                          fields = ["counter", "table"]}],
  emitrust.actor_locals = [{global = "LIMIT", name = "limit"}]} {
  emitrust.global @COUNTER : i32
  emitrust.global const @SCALE <3 : i32> : i32
  emitrust.global @TABLE : !emitrust.array<4xi32>
  emitrust.global @LIMIT <50 : i32> : i32

  // The arm: receiver prepended, member load/assign for the owned global,
  // opaque-constant path for the const, attribute consumed.
  // CHECK:      emitrust.impl "CounterActor" {
  // CHECK:        emitrust.func @bump(%[[SELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> i32 {
  // CHECK-NOT:    emitrust.actor_arm
  // CHECK:          %[[P:.*]] = emitrust.deref %[[SELF]]
  // CHECK:          %[[CNT:.*]] = emitrust.member %[[P]]["counter"]
  // CHECK:          %[[V0:.*]] = emitrust.load %[[CNT]]
  // CHECK:          %[[SC:.*]] = emitrust.constant <#emitrust.opaque<"SCALE">> : i32
  // CHECK:          %[[SUM:.*]] = emitrust.add %[[V0]], %[[SC]] : i32
  // CHECK:          emitrust.assign %[[CNT]] = %[[SUM]]
  // CHECK-NOT:      emitrust.global_load
  // CHECK-NOT:      emitrust.global_store
  emitrust.func @bump() -> i32 attributes {emitrust.actor_arm = "CounterActor"} {
    %0 = emitrust.global_load @COUNTER : i32
    %1 = emitrust.global_load @SCALE : i32
    %2 = emitrust.add %0, %1 : i32
    emitrust.global_store %2, @COUNTER : i32
    %3 = emitrust.global_load @COUNTER : i32
    emitrust.return %3 : i32
  }

  // The whole-global snapshot round-trip (variable filled from a
  // global_load, refined by subscript, written back by a global_store)
  // elides onto the direct member place: no staging variable, no
  // self-copy, one subscript assign against the member.
  // CHECK:        emitrust.func @poke_table(%[[TSELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>, %[[I:.*]]: i32)
  // CHECK:          %[[TP:.*]] = emitrust.deref %[[TSELF]]
  // CHECK:          %[[TAB:.*]] = emitrust.member %[[TP]]["table"]
  // CHECK-NOT:      emitrust.variable
  // CHECK:          %[[EL:.*]] = emitrust.subscript %[[TAB]][%[[I]]]
  // CHECK-NOT:      emitrust.load %[[TAB]]
  // CHECK:          emitrust.assign %[[EL]] = %[[I]] : !emitrust.lvalue<i32>
  // CHECK-NOT:      emitrust.assign %[[TAB]]
  // CHECK:          emitrust.return
  emitrust.func @poke_table(%arg0: i32)
      attributes {emitrust.actor_arm = "CounterActor",
                  emitrust.param_names = ["i"]} {
    %0 = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
    %1 = emitrust.global_load @TABLE : !emitrust.array<4xi32>
    emitrust.assign %0 = %1 : !emitrust.lvalue<!emitrust.array<4xi32>>
    %2 = emitrust.subscript %0[%arg0] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
    emitrust.assign %2 = %arg0 : !emitrust.lvalue<i32>
    %3 = emitrust.load %0 : (!emitrust.lvalue<!emitrust.array<4xi32>>) -> !emitrust.array<4xi32>
    emitrust.global_store %3, @TABLE : !emitrust.array<4xi32>
    emitrust.return
  }
  // CHECK:      }
  // CHECK:      emitrust.global const @SCALE <3 : i32> : i32

  // The driver: actor local named per the naming rule, no non-default
  // field init here (COUNTER/TABLE are zero-initialized); the driver-only
  // LIMIT becomes a named local CARRYING its initializer; the arm call is
  // a method_call on the local; the driver's own accesses go through the
  // member/local places; the module-level const load stays a global_load.
  // CHECK:      emitrust.func @c_main() -> i32 {
  // CHECK-NOT:    emitrust.actor_driver
  // CHECK-DAG:    %[[CA:.*]] = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.struct<"CounterActor">>
  // CHECK-DAG:    %[[LIM:.*]] = emitrust.variable named "limit" <50 : i32> : !emitrust.lvalue<i32>
  // CHECK:        %[[MC:.*]] = emitrust.member %[[CA]]["counter"]
  // CHECK:        %[[R:.*]] = emitrust.method_call %[[CA]]["bump"] () : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> i32
  // CHECK:        %[[LV:.*]] = emitrust.load %[[LIM]]
  // CHECK:        %[[S:.*]] = emitrust.add %[[R]], %[[LV]] : i32
  // CHECK:        emitrust.assign %[[LIM]] = %[[S]]
  // CHECK:        %[[CV:.*]] = emitrust.load %[[MC]]
  // CHECK:        %[[SCL:.*]] = emitrust.global_load @SCALE : i32
  // CHECK:        %[[T:.*]] = emitrust.add %[[CV]], %[[SCL]] : i32
  // CHECK:        emitrust.return %[[T]] : i32
  emitrust.func @c_main() -> i32 attributes {emitrust.actor_driver} {
    %0 = emitrust.call_opaque "bump"() : () -> i32
    %1 = emitrust.global_load @LIMIT : i32
    %2 = emitrust.add %0, %1 : i32
    emitrust.global_store %2, @LIMIT : i32
    %3 = emitrust.global_load @COUNTER : i32
    %4 = emitrust.global_load @SCALE : i32
    %5 = emitrust.add %3, %4 : i32
    emitrust.return %5 : i32
  }
}

// CHECK-NOT: emitrust.actor_lift
// CHECK-NOT: emitrust.actor_locals

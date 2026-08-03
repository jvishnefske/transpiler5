// FR-62 slice 4 (stage A): pins the pass-level DEMOTION guarantees — the
// invariants that (a) a module carrying no actor-lift attributes is left
// completely untouched (the default-off contract: without --actor-lift the
// pass in the pipeline tail is a no-op byte for byte), and (b) the
// safety-net veto: an owned global with any IR use OUTSIDE the planned
// actor surface (here a function carrying no role attribute — the shape a
// variadic monomorph or a recovered item produces) demotes the whole actor
// with a warning, keeping its global, its arms, and the driver call sites
// in today's thread-local form. Demotion-is-not-an-error: the pass
// succeeds and the surviving module is exactly the input minus the
// consumed attributes.
// RUN: emitrust-opt %s --split-input-file --emitrust-actor-lift 2>&1 \
// RUN:   | FileCheck %s

// The (b) veto warning is a located diagnostic on the demoted global; the
// diagnostic stream precedes the printed modules in the merged output.
// CHECK:      warning: actor lift: demoted CounterActor: global 'COUNTER' has a use outside the planned actor surface

// (a) No attributes: byte-for-byte no-op.
// CHECK:      emitrust.global @COUNTER : i32
// CHECK:      emitrust.func @bump() -> i32 {
// CHECK:        emitrust.global_load @COUNTER : i32
// CHECK-NOT:  emitrust.struct_def
module {
  emitrust.global @COUNTER : i32
  emitrust.func @bump() -> i32 {
    %0 = emitrust.global_load @COUNTER : i32
    emitrust.return %0 : i32
  }
}

// -----

// (b) The veto: @rogue touches COUNTER but carries no role attribute, so
// the actor is demoted — warning printed, global kept, arm kept at module
// level with its global accesses intact, driver call left an ordinary
// call_opaque, attributes consumed.
// CHECK:      emitrust.global @COUNTER : i32
// CHECK-NOT:  emitrust.struct_def
// CHECK-NOT:  emitrust.impl
// CHECK:      emitrust.func @bump() -> i32 {
// CHECK-NOT:    emitrust.actor_arm
// CHECK:        emitrust.global_load @COUNTER : i32
// CHECK:      emitrust.func @rogue() -> i32 {
// CHECK:        emitrust.global_load @COUNTER : i32
// CHECK:      emitrust.func @c_main() -> i32 {
// CHECK-NOT:    emitrust.actor_driver
// CHECK:        emitrust.call_opaque "bump"() : () -> i32
// CHECK-NOT:    emitrust.method_call
module attributes {
  emitrust.actor_lift = [{name = "CounterActor", var = "counter_actor",
                          globals = ["COUNTER"], fields = ["counter"]}]} {
  emitrust.global @COUNTER : i32
  emitrust.func @bump() -> i32 attributes {emitrust.actor_arm = "CounterActor"} {
    %0 = emitrust.global_load @COUNTER : i32
    emitrust.return %0 : i32
  }
  emitrust.func @rogue() -> i32 {
    %0 = emitrust.global_load @COUNTER : i32
    emitrust.return %0 : i32
  }
  emitrust.func @c_main() -> i32 attributes {emitrust.actor_driver} {
    %0 = emitrust.call_opaque "bump"() : () -> i32
    emitrust.return %0 : i32
  }
}

// -----

// (b') A plan symbol with no IR node is skip-if-absent: the actor's other
// global still lifts, and the absent one contributes no field.
// CHECK:      emitrust.struct_def @PairActor ["real"] [i32]
// CHECK:      emitrust.impl "PairActor"
// CHECK-NOT:  emitrust.global @REAL
module attributes {
  emitrust.actor_lift = [{name = "PairActor", var = "pair_actor",
                          globals = ["REAL", "FOLDED_AWAY"],
                          fields = ["real", "gone"]}]} {
  emitrust.global @REAL : i32
  emitrust.func @touch() -> i32 attributes {emitrust.actor_arm = "PairActor"} {
    %0 = emitrust.global_load @REAL : i32
    emitrust.return %0 : i32
  }
  emitrust.func @c_main() -> i32 attributes {emitrust.actor_driver} {
    %0 = emitrust.call_opaque "touch"() : () -> i32
    emitrust.return %0 : i32
  }
}

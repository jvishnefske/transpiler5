// FR-62 slice 4 (stage A): pins the pass-level DEMOTION guarantees — the
// invariants that (a) a module carrying no actor-lift attributes is left
// completely untouched (the disable contract: under --actor-lift=false —
// or stage A's default-off — no attributes are attached and the pass in
// the pipeline tail is a no-op byte for byte), and (b) the
// safety-net veto: an owned global with any IR use OUTSIDE the planned
// actor surface (here a function carrying no role attribute — the shape a
// variadic monomorph or a recovered item produces) demotes the whole actor
// with a warning, keeping its global, its arms, and the driver call sites
// in today's thread-local form. Demotion-is-not-an-error: the pass
// succeeds and the surviving module is exactly the input minus the
// consumed attributes.
// RUN: emitrust-opt %s --split-input-file --emitrust-actor-lift 2>&1 \
// RUN:   | FileCheck %s

// The veto warnings are located diagnostics on the demoted globals; the
// diagnostic stream precedes the printed modules in the merged output, so
// every case's warning is pinned here in case order.
// CHECK:      warning: actor lift: demoted CounterActor: global 'COUNTER' has a use outside the planned actor surface
// CHECK:      warning: actor lift: demoted fptr: global 'FPTR' carries an initializer with no local restatement (fn_ptr opaque init)
// CHECK:      warning: actor lift: demoted HookActor: global 'HOOK' carries an initializer with no local restatement (fn_ptr opaque init)

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

// -----

// (c) Stage-B regression (c-testsuite 00088): a fn_ptr global's opaque
// initializer (`None`) is legal on the GLOBAL op but has no VariableOp
// restatement — the driver-local lowering must veto it (warning pinned at
// the top, global kept in today's form, attribute consumed), never build
// an invalid `emitrust.variable`.
// CHECK:      emitrust.global @FPTR <#emitrust.opaque<"None">> : !emitrust.fn_ptr<() -> i32>
// CHECK:      emitrust.func @c_main() -> i32 {
// CHECK-NEXT:   emitrust.global_load @FPTR
module attributes {
  emitrust.actor_locals = [{global = "FPTR", name = "fptr"}]} {
  emitrust.global @FPTR <#emitrust.opaque<"None">> : !emitrust.fn_ptr<() -> i32>
  emitrust.func @c_main() -> i32 attributes {emitrust.actor_driver} {
    %0 = emitrust.global_load @FPTR : !emitrust.fn_ptr<() -> i32>
    %1 = emitrust.constant <#emitrust.opaque<"None">> : !emitrust.fn_ptr<() -> i32>
    %2 = emitrust.cmp  ne, %0, %1 : (!emitrust.fn_ptr<() -> i32>, !emitrust.fn_ptr<() -> i32>) -> i1
    %3 = emitrust.cast %2 : i1 to i32
    emitrust.return %3 : i32
  }
}

// -----

// (c') The same initializer veto on an ACTOR's owned global: the field-init
// staging variable would be just as invalid, so the whole actor demotes and
// its arm keeps the module-level thread-local form (warning pinned at the
// top).
// CHECK:      emitrust.global @HOOK <#emitrust.opaque<"None">> : !emitrust.fn_ptr<() -> i32>
// CHECK-NOT:  emitrust.struct_def
// CHECK-NOT:  emitrust.impl
// CHECK:      emitrust.func @poke() -> i32 {
// CHECK:        emitrust.global_load @HOOK
module attributes {
  emitrust.actor_lift = [{name = "HookActor", var = "hook_actor",
                          globals = ["HOOK"], fields = ["hook"]}]} {
  emitrust.global @HOOK <#emitrust.opaque<"None">> : !emitrust.fn_ptr<() -> i32>
  emitrust.func @poke() -> i32 attributes {emitrust.actor_arm = "HookActor"} {
    %0 = emitrust.global_load @HOOK : !emitrust.fn_ptr<() -> i32>
    %1 = emitrust.constant <#emitrust.opaque<"None">> : !emitrust.fn_ptr<() -> i32>
    %2 = emitrust.cmp  ne, %0, %1 : (!emitrust.fn_ptr<() -> i32>, !emitrust.fn_ptr<() -> i32>) -> i1
    %3 = emitrust.cast %2 : i1 to i32
    emitrust.return %3 : i32
  }
  emitrust.func @c_main() -> i32 attributes {emitrust.actor_driver} {
    %0 = emitrust.call_opaque "poke"() : () -> i32
    emitrust.return %0 : i32
  }
}

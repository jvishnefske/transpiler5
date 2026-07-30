// FR-52: turning the importer's external-requirement markers into a trait
// plus a type parameter on the transitive closure of their callers.
// RUN: emitrust-opt %s --split-input-file --emitrust-lower-external-requirements \
// RUN:   | FileCheck %s

// The trait lists the requirements in module order and is placed at the FRONT
// of the module, ahead of the code that consumes it.
// CHECK-LABEL: emitrust.trait_def @Externals ["host_scale", "host_bias"] [(i32) -> i32, () -> i32]
// CHECK-NOT:   emitrust.func @host_scale
// CHECK-NOT:   emitrust.func @host_bias
emitrust.func private @host_scale(i32) -> i32
    attributes {emitrust.external_requirement}
emitrust.func private @host_bias() -> i32
    attributes {emitrust.external_requirement}

// A direct caller becomes generic and routes the call through the parameter.
// CHECK:      emitrust.func @scaled
// CHECK-SAME:   emitrust.externals_generic = "Externals"
// CHECK:        emitrust.call_opaque "E::host_scale"
// CHECK:        emitrust.call_opaque "E::host_bias"
emitrust.func @scaled(%arg0: i32) -> i32 {
  %0 = emitrust.call_opaque "host_scale"(%arg0) : (i32) -> i32
  %1 = emitrust.call_opaque "host_bias"() : () -> i32
  %2 = emitrust.add %0, %1 : i32
  emitrust.return %2 : i32
}

// An INDIRECT caller becomes generic too and passes the parameter on: this is
// the transitive part of the rule.
// CHECK:      emitrust.func @twice
// CHECK-SAME:   emitrust.externals_generic = "Externals"
// CHECK:        emitrust.call_opaque "scaled::<E>"
emitrust.func @twice(%arg0: i32) -> i32 {
  %0 = emitrust.call_opaque "scaled"(%arg0) : (i32) -> i32
  emitrust.return %0 : i32
}

// A function that reaches no requirement is untouched: no attribute, and its
// callee spelling is unchanged.
// CHECK:      emitrust.func @pure
// CHECK-NOT:    emitrust.externals_generic
// CHECK:        emitrust.call_opaque "helper"
emitrust.func @pure(%arg0: i32) -> i32 {
  %0 = emitrust.call_opaque "helper"(%arg0) : (i32) -> i32
  emitrust.return %0 : i32
}

emitrust.func @helper(%arg0: i32) -> i32 {
  emitrust.return %arg0 : i32
}

// -----

// A module with no marked declaration is left exactly as it was: no trait, no
// attribute, no requalified callee. This is every module a project without
// unresolved externals produces, and it is what keeps the emitted crate
// byte-identical.
// CHECK-NOT: emitrust.trait_def
// CHECK-NOT: emitrust.externals_generic
// CHECK-LABEL: emitrust.func @only_local
// CHECK: emitrust.call_opaque "callee"
emitrust.func @only_local(%arg0: i32) -> i32 {
  %0 = emitrust.call_opaque "callee"(%arg0) : (i32) -> i32
  emitrust.return %0 : i32
}
emitrust.func @callee(%arg0: i32) -> i32 {
  emitrust.return %arg0 : i32
}

// -----

// Genericity propagates through an `emitrust.impl` member as well: the two
// call forms share one flat namespace of callee SYMBOLS, so a method is
// requalified with the same turbofish a free function gets.
// CHECK-LABEL: emitrust.trait_def @Externals
// CHECK:      emitrust.impl "Owner"
// CHECK:        emitrust.func @tick
// CHECK-SAME:     emitrust.externals_generic = "Externals"
// CHECK:          emitrust.call_opaque "E::host_reset"
// CHECK:      emitrust.func @driver
// CHECK-SAME:   emitrust.externals_generic = "Externals"
// CHECK:        emitrust.method_call %{{.*}}["tick::<E>"]
emitrust.func private @host_reset() attributes {emitrust.external_requirement}
emitrust.struct_def @Owner ["n"] [i32]
emitrust.impl "Owner" {
  emitrust.func @tick(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner">>) {
    emitrust.call_opaque "host_reset"() : () -> ()
    emitrust.return
  }
}
emitrust.func @driver() {
  %o = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Owner">>
  emitrust.method_call %o["tick"] () : (!emitrust.lvalue<!emitrust.struct<"Owner">>) -> ()
  emitrust.return
}

// -----

// Recursion converges rather than looping: the closure is a fixpoint, not a
// traversal of a DAG.
// CHECK-LABEL: emitrust.trait_def @Externals
// CHECK:      emitrust.func @recurse
// CHECK-SAME:   emitrust.externals_generic = "Externals"
// CHECK:        emitrust.call_opaque "recurse::<E>"
// CHECK:        emitrust.call_opaque "E::host_tick"
emitrust.func private @host_tick() attributes {emitrust.external_requirement}
emitrust.func @recurse(%arg0: i32) {
  emitrust.call_opaque "recurse"(%arg0) : (i32) -> ()
  emitrust.call_opaque "host_tick"() : () -> ()
  emitrust.return
}

// FR-70: turning the importer's external-requirement GLOBAL markers into
// getter/setter items on the same Externals trait FR-52 emits for functions,
// rewriting every whole-value load to a result-bearing `E::<getter>()` call
// expression and every store to an `E::set_<getter>(v)` call statement, and
// propagating genericity through the SAME caller fixpoint the function
// requirements use. This file pins that one trait and one fixpoint serve
// mixed fn+global requirements, that write-less globals emit only the
// getter (and read-less only the setter), that the item spelling is the
// snake_case of the emitted symbol, and that the marked global itself is
// erased only after every use has been rewritten.
// RUN: emitrust-opt %s --split-input-file --emitrust-lower-external-requirements \
// RUN:   | FileCheck %s

// Mixed function + global requirements share ONE trait, function items
// first, then each global's getter before its setter; the marked global is
// erased.
// CHECK-LABEL: emitrust.trait_def @Externals ["host_scale", "g_config", "set_g_config"] [(i32) -> i32, () -> i32, (i32) -> ()]
emitrust.func private @host_scale(i32) -> i32
    attributes {emitrust.external_requirement}

// A load becomes a result-bearing opaque call EXPRESSION sitting exactly
// where the load's value flowed; a store becomes a call STATEMENT.
// CHECK:      emitrust.func @bump
// CHECK-SAME:   emitrust.externals_generic = "Externals"
// CHECK:        %[[OLD:.*]] = emitrust.call_opaque "E::g_config"() : () -> i32
// CHECK:        %[[NEW:.*]] = emitrust.add %[[OLD]], %arg0 : i32
// CHECK:        emitrust.call_opaque "E::set_g_config"(%[[NEW]]) : (i32) -> ()
// CHECK-NOT:    emitrust.global_load
// CHECK-NOT:    emitrust.global_store
emitrust.func @bump(%arg0: i32) -> i32 {
  %0 = emitrust.global_load @g_config : i32
  %1 = emitrust.add %0, %arg0 : i32
  emitrust.global_store %1, @g_config : i32
  emitrust.return %0 : i32
}

// One function touching BOTH kinds of requirement is generic exactly once.
// CHECK:      emitrust.func @scaled
// CHECK-SAME:   emitrust.externals_generic = "Externals"
// CHECK:        emitrust.call_opaque "E::host_scale"
// CHECK:        emitrust.call_opaque "E::g_config"() : () -> i32
emitrust.func @scaled(%arg0: i32) -> i32 {
  %0 = emitrust.call_opaque "host_scale"(%arg0) : (i32) -> i32
  %1 = emitrust.global_load @g_config : i32
  %2 = emitrust.add %0, %1 : i32
  emitrust.return %2 : i32
}

// A global access is a caller edge like any call: the fixpoint carries it to
// INDIRECT callers, which pass the parameter on with the same turbofish.
// CHECK:      emitrust.func @twice
// CHECK-SAME:   emitrust.externals_generic = "Externals"
// CHECK:        emitrust.call_opaque "bump::<E>"
emitrust.func @twice(%arg0: i32) -> i32 {
  %0 = emitrust.call_opaque "bump"(%arg0) : (i32) -> i32
  emitrust.return %0 : i32
}

// A function that touches only an ORDINARY (defined, unmarked) global is
// outside the closure: no attribute, and its load is not rewritten. This is
// what keeps every project without global requirements byte-identical.
// CHECK:      emitrust.func @pure
// CHECK-NOT:    emitrust.externals_generic
// CHECK:        emitrust.global_load @plain : i32
emitrust.func @pure() -> i32 {
  %0 = emitrust.global_load @plain : i32
  emitrust.return %0 : i32
}

emitrust.global @plain <7 : i32> : i32
// The marked declaration-only global is gone; the unmarked one stays.
// CHECK:     emitrust.global @plain <7 : i32> : i32
// CHECK-NOT: emitrust.global @g_config
emitrust.global @g_config {emitrust.external_requirement} : i32

// -----

// A GLOBAL-ONLY module (no function requirement) still materializes the
// trait, and the getter spelling is the snake_case of the emitted SYMBOL:
// the importer under emitrust-cc marks `@G_CONFIG`, and both `G_CONFIG` and
// `g_config` derive the one item name `g_config` (the CSymbolNaming
// derivation, applied at a single point inside this pass).
// CHECK-LABEL: emitrust.trait_def @Externals ["g_config", "set_g_config"] [() -> i32, (i32) -> ()]
// CHECK:      emitrust.func @reset
// CHECK-SAME:   emitrust.externals_generic = "Externals"
// CHECK:        emitrust.call_opaque "E::g_config"() : () -> i32
// CHECK:        emitrust.call_opaque "E::set_g_config"(%{{.*}}) : (i32) -> ()
// CHECK-NOT:  emitrust.global @G_CONFIG
emitrust.func @reset() -> i32 {
  %0 = emitrust.global_load @G_CONFIG : i32
  %1 = emitrust.constant <0 : i32> : i32
  emitrust.global_store %1, @G_CONFIG : i32
  emitrust.return %0 : i32
}
emitrust.global @G_CONFIG {emitrust.external_requirement} : i32

// -----

// A WRITE-LESS global requirement emits only the getter: the item list is
// derived from the uses actually rewritten, so a setter nobody could call
// never appears in the trait.
// CHECK-LABEL: emitrust.trait_def @Externals ["g_counter"] [() -> i32]
// CHECK-NOT:  set_g_counter
emitrust.func @watch() -> i32 {
  %0 = emitrust.global_load @g_counter : i32
  emitrust.return %0 : i32
}
emitrust.global @g_counter {emitrust.external_requirement} : i32

// -----

// And symmetrically, a READ-LESS one emits only the setter.
// CHECK-LABEL: emitrust.trait_def @Externals ["set_g_flag"] [(i32) -> ()]
// CHECK:      emitrust.call_opaque "E::set_g_flag"(%{{.*}}) : (i32) -> ()
emitrust.func @raise() {
  %0 = emitrust.constant <1 : i32> : i32
  emitrust.global_store %0, @g_flag : i32
  emitrust.return
}
emitrust.global @g_flag {emitrust.external_requirement} : i32

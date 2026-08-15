// FR-70: turning the importer's external-requirement GLOBAL markers into
// getter/setter items on the same Externals trait FR-52 emits for functions,
// rewriting every whole-value load to a result-bearing `E::<getter>()` call
// expression and every store to an `E::set_<getter>(v)` call statement, and
// propagating genericity through the SAME caller fixpoint the function
// requirements use. This file pins that one trait and one fixpoint serve
// mixed fn+global requirements, that write-less globals emit only the
// getter (and read-less only the setter), that the item spelling is the
// snake_case of the emitted symbol, and that the marked global itself is
// erased only after every use has been rewritten. FR-79 extends the same
// machinery to CONST STRUCT requirements -- by-value struct getter, no
// setter, field projection on the bound Copy temporary -- with zero new
// rewrite shapes (the last case below pins that).
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

// -----

// FR-79: a CONST STRUCT requirement is getter-only by construction -- a
// const global has no stores, so the use-derived item list never grows a
// setter -- and the rewrite is the SAME load-to-call: the whole-value load
// becomes a result-bearing `E::<getter>()` whose Copy value binds exactly
// where the load's flowed, and field projection stays untouched on the
// bound temporary (the importer lowers `g.field` as load-then-member, so
// there is nothing global-specific left to rewrite). The trait item type is
// the struct BY VALUE.
// CHECK-LABEL: emitrust.trait_def @Externals ["ip_addr_any"] [() -> !emitrust.struct<"ip_addr">]
// CHECK-NOT:  set_ip_addr_any
// CHECK:      emitrust.func @is_any
// CHECK-SAME:   emitrust.externals_generic = "Externals"
// CHECK:        %[[V:.*]] = emitrust.call_opaque "E::ip_addr_any"() : () -> !emitrust.struct<"ip_addr">
// CHECK:        emitrust.assign %{{.*}} = %[[V]]
// CHECK:        emitrust.member %{{.*}}["addr"]
// CHECK-NOT:    emitrust.global_load
// CHECK-NOT:  emitrust.global @ip_addr_any
emitrust.struct_def @ip_addr ["addr", "kind"] [i32, i32]
emitrust.func @is_any(%arg0: i32) -> i32 {
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"ip_addr">>
  %1 = emitrust.global_load @ip_addr_any : !emitrust.struct<"ip_addr">
  emitrust.assign %0 = %1 : !emitrust.lvalue<!emitrust.struct<"ip_addr">>
  %2 = emitrust.member %0["addr"] : (!emitrust.lvalue<!emitrust.struct<"ip_addr">>) -> !emitrust.lvalue<i32>
  %3 = emitrust.load %2 : (!emitrust.lvalue<i32>) -> i32
  %4 = emitrust.cmp eq, %arg0, %3 : (i32, i32) -> i1
  %5 = emitrust.cast %4 : i1 to i32
  emitrust.return %5 : i32
}
emitrust.global const @ip_addr_any {emitrust.external_requirement} : !emitrust.struct<"ip_addr">

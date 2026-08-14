// FR-52 negatives: the shapes the lowering refuses rather than mis-emits.
// RUN: emitrust-opt %s --split-input-file --emitrust-lower-external-requirements \
// RUN:   --verify-diagnostics

// A C `struct Externals` would collide with the trait in Rust's item
// namespace.
emitrust.func private @host_tick() attributes {emitrust.external_requirement}
// expected-error @+1 {{unsupported: the project defines an item named 'Externals', which clashes with the external-requirement trait}}
emitrust.struct_def @Externals ["n"] [i32]
emitrust.func @caller() {
  emitrust.call_opaque "host_tick"() : () -> ()
  emitrust.return
}

// -----

// A C `struct E` is worse than a collision: a Rust type parameter SHADOWS a
// same-named type inside the generic item, so `E` in a generic signature
// would quietly stop meaning the struct.
emitrust.func private @host_tick() attributes {emitrust.external_requirement}
// expected-error @+1 {{unsupported: the project defines an item named 'E', which clashes with the external-requirement trait}}
emitrust.struct_def @E ["n"] [i32]
emitrust.func @caller() {
  emitrust.call_opaque "host_tick"() : () -> ()
  emitrust.return
}

// -----

// A module-level global holding the ADDRESS of a function that the trait made
// generic has nowhere to write the `::<E>` that address now needs -- a global
// initializer sits in no generic item. Refused rather than emitted as a
// `Some(scaled)` that names an item which no longer exists under that
// spelling.
emitrust.func private @host_scale(i32) -> i32
    attributes {emitrust.external_requirement}
emitrust.func @scaled(%arg0: i32) -> i32 {
  %0 = emitrust.call_opaque "host_scale"(%arg0) : (i32) -> i32
  emitrust.return %0 : i32
}
// expected-error @+1 {{unsupported: global 'hook' holds the address of 'scaled', which the external-requirement trait makes generic}}
emitrust.global @hook <#emitrust.opaque<"Some(scaled)">> : !emitrust.fn_ptr<(i32) -> i32>

// -----

// The address of a REQUIREMENT has no spelling at all: a trait method reached
// through the type parameter is not a function item, so `Some(E::f)` does not
// exist. The importer keeps such a symbol rejected, so this fires only on
// hand-written IR -- but it fires rather than emitting text naming nothing.
emitrust.func private @host_scale(i32) -> i32
    attributes {emitrust.external_requirement}
emitrust.func @take_address() -> !emitrust.fn_ptr<(i32) -> i32> {
  // expected-error @+1 {{unsupported: the address of external requirement 'host_scale' is taken; a trait method is not a function item}}
  %0 = emitrust.constant <#emitrust.opaque<"Some(host_scale)">> : !emitrust.fn_ptr<(i32) -> i32>
  %1 = emitrust.call_opaque "host_scale"(%0) : (!emitrust.fn_ptr<(i32) -> i32>) -> i32
  emitrust.return %0 : !emitrust.fn_ptr<(i32) -> i32>
}

// -----

// FR-70: a marked global whose use is NOT a whole-value load or store has no
// `E::g()` / `E::set_g(v)` spelling -- here a cell-slice borrow, which needs
// the PLACE the trait deliberately does not model. The importer's gate never
// marks such a shape, so like the fn-pointer case above this fires only on
// hand-written IR -- but it fires BEFORE any edit, rather than erasing a
// global out from under a use the rewrite cannot express.
emitrust.global @table {emitrust.external_requirement} : !emitrust.array<4xi32>
emitrust.func @scan() {
  // expected-error @+1 {{unsupported: external-requirement global 'table' has a use that is not a whole-value load or store}}
  emitrust.global_cells @table {
  ^bb0(%cells: !emitrust.ref<!emitrust.cell_slice<i32>>):
    emitrust.yield
  }
  emitrust.return
}

// -----

// FR-70: two globals whose emitted symbols collapse to one snake_case
// accessor would emit a trait with two same-named items, which Rust rejects
// (or resolves by shadowing -- either way not the two distinct storages the
// C program had). Renaming is not an option for the same reason the
// Externals/E clash is not worked around: the emitted API would silently
// depend on an unrelated C identifier. Refused before any edit.
emitrust.global @G_CONFIG {emitrust.external_requirement} : i32
// expected-error @+1 {{unsupported: external requirement 'g_config' derives trait item 'g_config', which collides with the item derived from 'G_CONFIG'}}
emitrust.global @g_config {emitrust.external_requirement} : i32
emitrust.func @touch() -> i32 {
  %0 = emitrust.global_load @G_CONFIG : i32
  %1 = emitrust.global_load @g_config : i32
  %2 = emitrust.add %0, %1 : i32
  emitrust.return %2 : i32
}

// -----

// FR-70: the derived accessor names share the trait's single namespace with
// the FUNCTION requirements, so a getter colliding with a required function
// is refused the same way.
emitrust.func private @g_config() -> i32
    attributes {emitrust.external_requirement}
// expected-error @+1 {{unsupported: external requirement 'G_CONFIG' derives trait item 'g_config', which collides with the item derived from 'g_config'}}
emitrust.global @G_CONFIG {emitrust.external_requirement} : i32
emitrust.func @touch() -> i32 {
  %0 = emitrust.call_opaque "g_config"() : () -> i32
  %1 = emitrust.global_load @G_CONFIG : i32
  %2 = emitrust.add %0, %1 : i32
  emitrust.return %2 : i32
}

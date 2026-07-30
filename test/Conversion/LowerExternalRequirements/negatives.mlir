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

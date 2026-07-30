// FR-52: rendering the external-requirement trait and the functions that are
// generic over it.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// The trait is ALWAYS `pub`, even here where nothing asked for exported
// items: a requirement the crate's consumer cannot name is a requirement
// nobody can satisfy.
// CHECK:      pub trait Externals {
// CHECK-NEXT:     fn host_scale(v0: i32) -> i32;
// CHECK-NEXT:     fn host_reset();
// CHECK-NEXT:     fn host_mix(v0: i32, v1: f64) -> f64;
// CHECK-NEXT: }
emitrust.trait_def @Externals ["host_scale", "host_reset", "host_mix"]
    [(i32) -> i32, () -> (), (i32, f64) -> f64]

// A function in the closure carries the bound; the call sites were already
// requalified by the lowering pass, so the emitter only renders them.
// CHECK:      fn scaled<E: Externals>(v0: i32) -> i32 {
// CHECK-NEXT:     let v1: i32 = E::host_scale(v0);
// CHECK-NEXT:     return v1;
// CHECK-NEXT: }
emitrust.func @scaled(%arg0: i32) -> i32
    attributes {emitrust.externals_generic = "Externals"} {
  %0 = emitrust.call_opaque "E::host_scale"(%arg0) : (i32) -> i32
  emitrust.return %0 : i32
}

// A function that needs nothing keeps exactly the signature it always had --
// no type parameter, no turbofish.
// CHECK:      fn pure(v0: i32) -> i32 {
// CHECK-NEXT:     return v0;
// CHECK-NEXT: }
emitrust.func @pure(%arg0: i32) -> i32 {
  emitrust.return %arg0 : i32
}

// A generic METHOD renders inside its impl, and its call site turbofishes.
// CHECK:      impl Owner {
// CHECK-NEXT:     fn tick<E: Externals>(&mut self) {
// CHECK-NEXT:         E::host_reset();
emitrust.struct_def @Owner ["n"] [i32]
emitrust.impl "Owner" {
  emitrust.func @tick(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner">>)
      attributes {emitrust.externals_generic = "Externals"} {
    emitrust.call_opaque "E::host_reset"() : () -> ()
    emitrust.return
  }
}

// CHECK:      fn driver<E: Externals>() {
// CHECK:          let {{.*}} = scaled::<E>({{.*}});
// CHECK:          {{.*}}.tick::<E>();
emitrust.func @driver() attributes {emitrust.externals_generic = "Externals"} {
  %c = emitrust.constant <1 : i32> : i32
  %0 = emitrust.call_opaque "scaled::<E>"(%c) : (i32) -> i32
  %o = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Owner">>
  emitrust.method_call %o["tick::<E>"] () : (!emitrust.lvalue<!emitrust.struct<"Owner">>) -> ()
  emitrust.return
}

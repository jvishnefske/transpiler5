// FR-52: rendering the external-requirement trait and the functions that are
// generic over it. FR-70 adds GLOBAL requirements to the same trait as a
// getter/setter pair of ordinary associated functions -- `fn g_config() ->
// i32` / `fn set_g_config(v0: i32)` -- so this golden also pins that exact
// spelling and the expression/statement rendering of their call sites.
// FR-79 adds the BY-VALUE STRUCT getter of a const-struct requirement --
// `fn cfg() -> S;` -- and pins that its call site binds a Copy temporary on
// which field projection is ordinary member access.
// FR-81 adds the BY-VALUE STRUCT SETTER of a NON-const struct requirement --
// `fn set_cfg(v0: S);` -- and pins the staged read-modify-write call site:
// getter call, member-assign on the bound Copy temporary, whole-value
// setter call STATEMENT (never a projection into environment storage).
// FR-80 adds the ADDRESS-CARRYING getter -- a trait-item RESULT of
// !emitrust.ref type renders `&'static T` (a bare `&` in a zero-arg trait
// fn signature is rustc E0106; the requirement address is the consumer's
// static item, so 'static is the truthful lifetime). The spelling is
// confined to trait-item results: everywhere else a ref stays a bare `&`,
// as the call-site bindings below pin.
// FR-85 adds the BY-VALUE BYTE-REGION getter of a const byte-region
// requirement -- `fn ethbroadcast() -> [u8; 6];` -- an array-typed item
// result needing no special rendering (Rust arrays are Copy) -- plus the
// byte-slice-parameter item shape (`fn eth_out(v0: &[u8]);`) a body-less
// byte-region-pointee callee lowers to, and pins the call site: the
// getter's value seeds a temporary on which subscripting and the shared
// byte slice are ordinary, exactly the importer's staged-copy image.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// The trait is ALWAYS `pub`, even here where nothing asked for exported
// items: a requirement the crate's consumer cannot name is a requirement
// nobody can satisfy. A global's getter/setter items need no special
// rendering at all: they are `() -> T` and `(T) -> ()` associated functions
// like any other -- including FR-79's struct-returning getter, whose `T` is
// just a struct type.
// CHECK:      pub trait Externals {
// CHECK-NEXT:     fn host_scale(v0: i32) -> i32;
// CHECK-NEXT:     fn host_reset();
// CHECK-NEXT:     fn host_mix(v0: i32, v1: f64) -> f64;
// CHECK-NEXT:     fn g_config() -> i32;
// CHECK-NEXT:     fn set_g_config(v0: i32);
// CHECK-NEXT:     fn cfg() -> S;
// CHECK-NEXT:     fn set_cfg(v0: S);
// CHECK-NEXT:     fn any() -> &'static S;
// CHECK-NEXT:     fn eth_out(v0: &[u8]);
// CHECK-NEXT:     fn ethbroadcast() -> [u8; 6];
// CHECK-NEXT: }
emitrust.trait_def @Externals
    ["host_scale", "host_reset", "host_mix", "g_config", "set_g_config", "cfg", "set_cfg", "any", "eth_out", "ethbroadcast"]
    [(i32) -> i32, () -> (), (i32, f64) -> f64, () -> i32, (i32) -> (), () -> !emitrust.struct<"S">, (!emitrust.struct<"S">) -> (), () -> !emitrust.ref<!emitrust.struct<"S">>, (!emitrust.ref<!emitrust.slice<ui8>>) -> (), () -> !emitrust.array<6xui8>]

// FR-70: a getter call is a result-bearing opaque call, so its value binds
// (or folds) exactly where the global load's value flowed; a setter call is
// a statement. Nothing here is global-specific by the time it renders --
// which is the point: the pass lowered storage access to plain calls.
// CHECK:      fn bump<E: Externals>(v0: i32) -> i32 {
// CHECK-NEXT:     let v1: i32 = E::g_config();
// CHECK-NEXT:     E::set_g_config(v1 + v0);
// CHECK-NEXT:     v1
// CHECK-NEXT: }
emitrust.func @bump(%arg0: i32) -> i32
    attributes {emitrust.externals_generic = "Externals"} {
  %0 = emitrust.call_opaque "E::g_config"() : () -> i32
  %1 = emitrust.add %0, %arg0 : i32
  emitrust.call_opaque "E::set_g_config"(%1) : (i32) -> ()
  emitrust.return %0 : i32
}

// FR-79: the struct getter's call site is nothing special either -- the
// result-bearing opaque call binds a Copy temporary of the struct type, and
// the field read is plain member access on that temporary (never on the
// erased global). The struct itself renders with the usual Copy derive,
// which is what makes the by-value return a faithful read.
// CHECK:      struct S {
// CHECK:      fn read_cfg<E: Externals>() -> i32 {
// CHECK-NEXT:     let v0: S = E::cfg();
// CHECK-NEXT:     let v1: S;
// CHECK-NEXT:     v1 = v0;
// CHECK-NEXT:     v1.x
// CHECK-NEXT: }
emitrust.struct_def @S ["x", "y"] [i32, i32]
emitrust.func @read_cfg() -> i32
    attributes {emitrust.externals_generic = "Externals"} {
  %0 = emitrust.call_opaque "E::cfg"() : () -> !emitrust.struct<"S">
  %1 = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"S">>
  emitrust.assign %1 = %0 : !emitrust.lvalue<!emitrust.struct<"S">>
  %2 = emitrust.member %1["x"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<i32>
  %3 = emitrust.load %2 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %3 : i32
}

// FR-81: the setter's call site is the staged read-modify-write the importer
// emits for a field store through a non-const struct requirement: the
// getter's Copy value seeds a mutable temporary, the member-assign lands on
// the temporary, and the setter call renders as a STATEMENT carrying the
// whole value -- which is why no dead-store analysis can touch it (a call
// statement is never an elision candidate; external storage stays
// observable).
// CHECK:      fn write_cfg<E: Externals>(v0: i32) {
// CHECK-NEXT:     let mut v1: S;
// CHECK-NEXT:     let v2: S = E::cfg();
// CHECK-NEXT:     v1 = v2;
// CHECK-NEXT:     v1.x = v0;
// CHECK-NEXT:     E::set_cfg(v1);
// CHECK-NEXT: }
emitrust.func @write_cfg(%arg0: i32)
    attributes {emitrust.externals_generic = "Externals"} {
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"S">>
  %1 = emitrust.call_opaque "E::cfg"() : () -> !emitrust.struct<"S">
  emitrust.assign %0 = %1 : !emitrust.lvalue<!emitrust.struct<"S">>
  %2 = emitrust.member %0["x"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<i32>
  emitrust.assign %2 = %arg0 : !emitrust.lvalue<i32>
  %3 = emitrust.load %0 : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.struct<"S">
  emitrust.call_opaque "E::set_cfg"(%3) : (!emitrust.struct<"S">) -> ()
  emitrust.return
}

// FR-80: the address getter's call site binds an ordinary `&S` value — the
// 'static spelling exists ONLY in the trait item — and everything after it
// is the existing reference machinery: field reads auto-deref through the
// binding, the interior-member address (the lwIP IP4_ADDR_ANY chain) is a
// plain shared reborrow of the projected member, and the ref value passes
// straight into a `&S` parameter.
// CHECK:      fn read_any<E: Externals>() -> i32 {
// CHECK-NEXT:     let v0: &S = E::any();
// CHECK-NEXT:     let v1: &i32 = &v0.x;
// CHECK-NEXT:     v0.y + *v1
// CHECK-NEXT: }
emitrust.func @read_any() -> i32
    attributes {emitrust.externals_generic = "Externals"} {
  %0 = emitrust.call_opaque "E::any"() : () -> !emitrust.ref<!emitrust.struct<"S">>
  %1 = emitrust.deref %0 : (!emitrust.ref<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.struct<"S">>
  %2 = emitrust.member %1["x"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<i32>
  %3 = emitrust.addr_of %2 : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
  %4 = emitrust.member %1["y"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<i32>
  %5 = emitrust.load %4 : (!emitrust.lvalue<i32>) -> i32
  %6 = emitrust.deref %3 : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
  %7 = emitrust.load %6 : (!emitrust.lvalue<i32>) -> i32
  %8 = emitrust.add %5, %7 : i32
  emitrust.return %8 : i32
}

// FR-85: the byte-region getter's call site is the importer's staged-copy
// image rendered plainly -- the array-valued opaque call binds a Copy
// temporary, and both the element read and the shared byte slice
// (`slice_of`, the argument-pass shape) work on that TEMPORARY, never on
// environment-owned storage.
// CHECK:      fn eth_first<E: Externals>() -> i32 {
// CHECK-NEXT:     let v0: [u8; 6] = E::ethbroadcast();
// CHECK-NEXT:     let v1: [u8; 6];
// CHECK-NEXT:     v1 = v0;
// CHECK-NEXT:     let v3: &[u8] = &v1[0i64 as usize..];
// CHECK-NEXT:     E::eth_out(v3);
// CHECK-NEXT:     v1[0i64 as usize] as i32
// CHECK-NEXT: }
emitrust.func @eth_first() -> i32
    attributes {emitrust.externals_generic = "Externals"} {
  %0 = emitrust.call_opaque "E::ethbroadcast"() : () -> !emitrust.array<6xui8>
  %1 = emitrust.variable : !emitrust.lvalue<!emitrust.array<6xui8>>
  emitrust.assign %1 = %0 : !emitrust.lvalue<!emitrust.array<6xui8>>
  %c0 = emitrust.constant <0 : i64> : i64
  %2 = emitrust.slice_of %1[%c0] : (!emitrust.lvalue<!emitrust.array<6xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
  emitrust.call_opaque "E::eth_out"(%2) : (!emitrust.ref<!emitrust.slice<ui8>>) -> ()
  %3 = emitrust.subscript %1[%c0] : (!emitrust.lvalue<!emitrust.array<6xui8>>, i64) -> !emitrust.lvalue<ui8>
  %4 = emitrust.load %3 : (!emitrust.lvalue<ui8>) -> ui8
  %5 = emitrust.cast %4 : ui8 to i32
  emitrust.return %5 : i32
}

// A function in the closure carries the bound; the call sites were already
// requalified by the lowering pass, so the emitter only renders them.
// CHECK:      fn scaled<E: Externals>(v0: i32) -> i32 {
// CHECK-NEXT:     E::host_scale(v0)
// CHECK-NEXT: }
emitrust.func @scaled(%arg0: i32) -> i32
    attributes {emitrust.externals_generic = "Externals"} {
  %0 = emitrust.call_opaque "E::host_scale"(%arg0) : (i32) -> i32
  emitrust.return %0 : i32
}

// A function that needs nothing keeps exactly the signature it always had --
// no type parameter, no turbofish.
// CHECK:      fn pure(v0: i32) -> i32 {
// CHECK-NEXT:     v0
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

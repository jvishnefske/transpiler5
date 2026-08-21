// W2.17: Rust rendering of the Drop surface. Three emission decisions,
// each gated on an attribute that no pre-W2.17 module can carry, so every
// existing program is byte-identical to before:
//
// 1. an `emitrust.impl` carrying `trait_name = "Drop"` renders
//    `impl Drop for <Struct>` instead of the inherent `impl <Struct>`;
// 2. an `emitrust.struct_def` carrying `emitrust.has_drop` loses `Copy`
//    from its derive list -- measured rustc E0184: "the trait `Copy`
//    cannot be implemented for this type; the type has a destructor" --
//    while `Clone` and `Default` stay (both compile fine alongside Drop,
//    and no `.clone()` is ever emitted, so `Clone` is inert);
// 3. the inherent impl for the SAME struct still renders normally, and a
//    struct may carry both -- two impls per struct are legal.
//
// Two classes each with their own `@drop` prove the per-impl SymbolTable
// keeps the two `drop` symbols apart (a module-level symbol table could
// not hold both).
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK: #[derive(Clone, Default)]
// CHECK-NEXT: struct R {
emitrust.struct_def @R ["id"] [i32] {emitrust.has_drop}
// A struct with no destructor keeps Copy: the attribute, not the presence
// of some other impl, is what decides.
// CHECK: #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct Plain {
emitrust.struct_def @Plain ["id"] [i32]
// CHECK: #[derive(Clone, Default)]
// CHECK-NEXT: struct S {
emitrust.struct_def @S ["id"] [i32] {emitrust.has_drop}

// CHECK: impl R {
// CHECK-NEXT: fn r_new(&mut self, v0: i32) {
emitrust.impl "R" {
  emitrust.func @r_new(%self: !emitrust.mut_ref<!emitrust.struct<"R">>, %i: i32) {
    %0 = emitrust.deref %self : (!emitrust.mut_ref<!emitrust.struct<"R">>) -> !emitrust.lvalue<!emitrust.struct<"R">>
    %1 = emitrust.member %0["id"] : (!emitrust.lvalue<!emitrust.struct<"R">>) -> !emitrust.lvalue<i32>
    emitrust.assign %1 = %i : !emitrust.lvalue<i32>
    emitrust.return
  }
}

// CHECK: impl Drop for R {
// CHECK-NEXT: fn drop(&mut self) {
// CHECK-NEXT: println!("dtor R {}", self.id);
emitrust.impl "R" {
  emitrust.func @drop(%self: !emitrust.mut_ref<!emitrust.struct<"R">>) {
    %0 = emitrust.deref %self : (!emitrust.mut_ref<!emitrust.struct<"R">>) -> !emitrust.lvalue<!emitrust.struct<"R">>
    %1 = emitrust.member %0["id"] : (!emitrust.lvalue<!emitrust.struct<"R">>) -> !emitrust.lvalue<i32>
    %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
    emitrust.call_opaque "println!"(%2) {args = ["dtor R {}", 0 : index]} : (i32) -> ()
    emitrust.return
  }
} {trait_name = "Drop"}

// CHECK: impl Drop for S {
// CHECK-NEXT: fn drop(&mut self) {
emitrust.impl "S" {
  emitrust.func @drop(%self: !emitrust.mut_ref<!emitrust.struct<"S">>) {
    %0 = emitrust.deref %self : (!emitrust.mut_ref<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.struct<"S">>
    %1 = emitrust.member %0["id"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<i32>
    %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
    emitrust.call_opaque "println!"(%2) {args = ["dtor S {}", 0 : index]} : (i32) -> ()
    emitrust.return
  }
} {trait_name = "Drop"}

// The trait name is never emitted as an inherent impl header, and the
// trait-impl member never takes a visibility prefix (rustc E0449 forbids
// `pub` on a trait-impl member; the binary-crate rendering carries none
// either way, and the lib-crate rendering is pinned in
// test/Driver/emit-crate-lib-drop.cpp).
// CHECK-NOT: impl Drop {

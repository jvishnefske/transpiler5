// W2.17: `convert-func-to-emitrust` routes an imported C++ destructor into
// its class's `Drop` impl. Three things must all happen together, and this
// is the only place any of them is decided:
//
// 1. the `emitrust.drop_impl`-marked function goes into a SECOND
//    `emitrust.impl` for the same struct, carrying `trait_name = "Drop"` --
//    a name-only impl lookup merges it into the INHERENT impl (measured),
//    where it would render as an ordinary method, not a Drop impl;
// 2. it is RENAMED to the symbol `drop` (rustc E0407 otherwise). The rename
//    is safe because `emitrust.impl` is a SymbolTable and nothing in the
//    subset ever calls a destructor;
// 3. both placement markers are consumed -- `emitrust.method_of` as always,
//    and `emitrust.drop_impl`, whose job the trait impl now records
//    structurally.
//
// `Owner_drop` below is a USER member function spelled `drop`: it must keep
// its own name in the inherent impl, which is exactly why the destructor's
// module symbol is `Owner_dtor` rather than `Owner_drop`.
// RUN: emitrust-opt --convert-func-to-emitrust %s | FileCheck %s

// CHECK: emitrust.impl "Owner" {
// CHECK-NOT: trait_name
// CHECK: emitrust.func @Owner_set
// CHECK: emitrust.func @Owner_drop
func.func @Owner_set(%self: !emitrust.mut_ref<!emitrust.struct<"Owner">>, %v: i32)
    attributes {emitrust.method_of = "Owner"} {
  %p = emitrust.deref %self : (!emitrust.mut_ref<!emitrust.struct<"Owner">>) -> !emitrust.lvalue<!emitrust.struct<"Owner">>
  %f = emitrust.member %p["id"] : (!emitrust.lvalue<!emitrust.struct<"Owner">>) -> !emitrust.lvalue<i32>
  emitrust.assign %f = %v : !emitrust.lvalue<i32>
  return
}

func.func @Owner_drop(%self: !emitrust.mut_ref<!emitrust.struct<"Owner">>)
    attributes {emitrust.method_of = "Owner"} {
  return
}

// The destructor: renamed to `drop`, in its own trait-carrying impl, with
// both markers consumed.
// CHECK: emitrust.impl "Owner" {
// CHECK-NEXT: emitrust.func @drop(%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"Owner">>) {
// CHECK-NOT: emitrust.drop_impl
// CHECK-NOT: emitrust.method_of
// CHECK: } {trait_name = "Drop"}
// CHECK-NOT: emitrust.func @Owner_dtor
func.func @Owner_dtor(%self: !emitrust.mut_ref<!emitrust.struct<"Owner">>)
    attributes {emitrust.method_of = "Owner", emitrust.drop_impl} {
  return
}

// W2.17: round-trip pin for the Drop surface. The wave adds NO new op and
// NO assembly-format change: `trait_name` is an optional inherent
// attribute that ODS routes through `attr-dict`, and `emitrust.has_drop`
// is a plain discardable unit attribute on `emitrust.struct_def`. Both
// must survive parse -> print verbatim, and two `emitrust.impl` ops for
// the SAME struct (one inherent, one trait) must verify -- that is what
// makes `impl R { .. }` and `impl Drop for R { .. }` coexist.
// RUN: emitrust-opt %s | emitrust-opt | FileCheck %s

// CHECK: emitrust.struct_def @R ["id"] [i32] {emitrust.has_drop}
emitrust.struct_def @R ["id"] [i32] {emitrust.has_drop}

// CHECK: emitrust.impl "R" {
// CHECK-NEXT: emitrust.func @r_new
emitrust.impl "R" {
  emitrust.func @r_new(%self: !emitrust.mut_ref<!emitrust.struct<"R">>, %i: i32) {
    %0 = emitrust.deref %self : (!emitrust.mut_ref<!emitrust.struct<"R">>) -> !emitrust.lvalue<!emitrust.struct<"R">>
    %1 = emitrust.member %0["id"] : (!emitrust.lvalue<!emitrust.struct<"R">>) -> !emitrust.lvalue<i32>
    emitrust.assign %1 = %i : !emitrust.lvalue<i32>
    emitrust.return
  }
}

// CHECK: emitrust.impl "R" {
// CHECK-NEXT: emitrust.func @drop
// CHECK: } {trait_name = "Drop"}
emitrust.impl "R" {
  emitrust.func @drop(%self: !emitrust.mut_ref<!emitrust.struct<"R">>) {
    emitrust.return
  }
} {trait_name = "Drop"}

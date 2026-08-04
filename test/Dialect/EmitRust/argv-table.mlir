// C99-43 C3: parse/print round-trip of the argv-table surface. The
// `!emitrust.argv_table` type is the opaque command-line argument table
// of C main's admitted `char **argv` parameter (rendered `&[Vec<i8>]`);
// it is valid only as a function parameter and its single consumer is
// `emitrust.argv_arg`, which borrows one argument's NUL-terminated byte
// run as `!emitrust.ref<!emitrust.slice<i8>>`. From there the ordinary
// slice surface (deref + subscript + load) reads single bytes. This test
// pins the textual assembly of both the type and the op so importer
// goldens and the emitter agree on one spelling.
// RUN: emitrust-opt %s | emitrust-opt | FileCheck %s

// CHECK-LABEL: emitrust.func @c_main(
// CHECK-SAME: i32
// CHECK-SAME: !emitrust.argv_table
emitrust.func @c_main(%arg0: i32, %arg1: !emitrust.argv_table) -> i32 {
  // An i32 index carries the `as usize` rendering downstream.
  %i = emitrust.constant <0 : i32> : i32
  // CHECK: %[[S:.*]] = emitrust.argv_arg %{{.*}}[%{{.*}}] : (!emitrust.argv_table, i32) -> !emitrust.ref<!emitrust.slice<i8>>
  %s = emitrust.argv_arg %arg1[%i] : (!emitrust.argv_table, i32) -> !emitrust.ref<!emitrust.slice<i8>>
  // The borrowed run reads through the EXISTING slice surface: deref to
  // the slice place, subscript a byte, load it.
  // CHECK: emitrust.deref %[[S]]
  %p = emitrust.deref %s : (!emitrust.ref<!emitrust.slice<i8>>) -> !emitrust.lvalue<!emitrust.slice<i8>>
  %j = emitrust.constant <1 : i64> : i64
  // CHECK: emitrust.subscript
  %e = emitrust.subscript %p[%j] : (!emitrust.lvalue<!emitrust.slice<i8>>, i64) -> !emitrust.lvalue<i8>
  // CHECK: emitrust.load
  %v = emitrust.load %e : (!emitrust.lvalue<i8>) -> i8
  // The whole run feeds the on-demand raw stdout helper by value.
  // CHECK: emitrust.call_opaque "__emitrust_cstr_out"(%[[S]])
  emitrust.call_opaque "__emitrust_cstr_out"(%s) : (!emitrust.ref<!emitrust.slice<i8>>) -> ()
  emitrust.return %arg0 : i32
}

// An index-typed index round-trips too (no cast is rendered for it).
// CHECK-LABEL: emitrust.func @index_typed
emitrust.func @index_typed(%arg0: !emitrust.argv_table, %arg1: index) {
  // CHECK: emitrust.argv_arg %{{.*}}[%{{.*}}] : (!emitrust.argv_table, index) -> !emitrust.ref<!emitrust.slice<i8>>
  %s = emitrust.argv_arg %arg0[%arg1] : (!emitrust.argv_table, index) -> !emitrust.ref<!emitrust.slice<i8>>
  emitrust.return
}

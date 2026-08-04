// C99-43 C3: Rust rendering of the argv-table surface. An
// `!emitrust.argv_table` parameter renders as `&[Vec<i8>]` (the borrow is
// part of the type's rendering — the table only ever appears as a
// parameter); `emitrust.argv_arg` renders as the whole-vector slice
// borrow `&table[i as usize][..]` (`as usize` omitted for an index-typed
// index), from which the ordinary slice surface — deref + subscript for
// byte reads, a helper call for the whole run — renders exactly as it
// does for any other `&[i8]`. Pinned here so the importer's argv lowering
// and the crate wrapper (`Vec<Vec<i8>>` collection) agree on the bytes.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK: fn c_main(v0: i32, v1: &[Vec<i8>]) -> i32 {
emitrust.func @c_main(%arg0: i32, %arg1: !emitrust.argv_table) -> i32 {
  %i = emitrust.constant <0 : i32> : i32
  // CHECK: let v3: &[i8] = &v1[0i32 as usize][..];
  %s = emitrust.argv_arg %arg1[%i] : (!emitrust.argv_table, i32) -> !emitrust.ref<!emitrust.slice<i8>>
  // CHECK: __emitrust_cstr_out(v3);
  emitrust.call_opaque "__emitrust_cstr_out"(%s) : (!emitrust.ref<!emitrust.slice<i8>>) -> ()
  %p = emitrust.deref %s : (!emitrust.ref<!emitrust.slice<i8>>) -> !emitrust.lvalue<!emitrust.slice<i8>>
  %j = emitrust.constant <1 : i64> : i64
  // The byte read renders through the shared place machinery —
  // `(*ref)[j as usize]` — and the single-use load folds inline into
  // the consuming call (FR-61d).
  // CHECK: __emitrust_byte_out((*v3)[1i64 as usize]);
  %e = emitrust.subscript %p[%j] : (!emitrust.lvalue<!emitrust.slice<i8>>, i64) -> !emitrust.lvalue<i8>
  %v = emitrust.load %e : (!emitrust.lvalue<i8>) -> i8
  emitrust.call_opaque "__emitrust_byte_out"(%v) : (i8) -> ()
  emitrust.return %arg0 : i32
}

// An index-typed index renders with no cast.
// CHECK: fn index_typed(v0: &[Vec<i8>], v1: usize) {
emitrust.func @index_typed(%arg0: !emitrust.argv_table, %arg1: index) {
  // CHECK: let v2: &[i8] = &v0[v1][..];
  %s = emitrust.argv_arg %arg0[%arg1] : (!emitrust.argv_table, index) -> !emitrust.ref<!emitrust.slice<i8>>
  emitrust.call_opaque "__emitrust_cstr_out"(%s) : (!emitrust.ref<!emitrust.slice<i8>>) -> ()
  emitrust.return
}

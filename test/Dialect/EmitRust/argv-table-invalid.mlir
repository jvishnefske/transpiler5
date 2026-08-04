// C99-43 C3: verifier rejections of the argv-table surface. The
// `emitrust.argv_arg` result is pinned to the ONE run an argv table
// stores — a shared reference to a byte slice — so a wrong element type
// or a wrong reference shape is a verifier error, never a silently
// mistyped borrow reaching the Rust emitter.
// RUN: emitrust-opt %s --split-input-file --verify-diagnostics

emitrust.func @wrong_element(%arg0: !emitrust.argv_table, %arg1: i32) {
  // expected-error @+1 {{result must be a !emitrust.ref of !emitrust.slice<i8>}}
  %s = emitrust.argv_arg %arg0[%arg1] : (!emitrust.argv_table, i32) -> !emitrust.ref<!emitrust.slice<i32>>
  emitrust.return
}

// -----

emitrust.func @not_a_slice(%arg0: !emitrust.argv_table, %arg1: i32) {
  // expected-error @+1 {{result must be a !emitrust.ref of !emitrust.slice<i8>}}
  %s = emitrust.argv_arg %arg0[%arg1] : (!emitrust.argv_table, i32) -> !emitrust.ref<i8>
  emitrust.return
}

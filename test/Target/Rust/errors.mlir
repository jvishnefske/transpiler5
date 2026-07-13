// FR-9: Unsupported constructs fail translation with a located diagnostic, not silent bad output.
// RUN: not emitrust-translate --mlir-to-rust --split-input-file %s 2>&1 | FileCheck %s

// An unsupported (tensor) type must be diagnosed.
emitrust.func @unsupported_type() {
  // CHECK: cannot translate type
  %0 = emitrust.literal "x" : tensor<4xi32>
  emitrust.return
}

// -----

// An op from a foreign dialect must be diagnosed.
emitrust.func @unsupported_op(%arg0: i32) {
  // CHECK: unable to translate op
  %0 = builtin.unrealized_conversion_cast %arg0 : i32 to i64
  emitrust.return
}

// -----

// Rust has no `as bool` cast; a boolean-producing cast must be diagnosed.
emitrust.func @cast_to_bool(%arg0: i32) {
  // CHECK: cannot translate a cast to bool
  %0 = emitrust.cast %arg0 : i32 to i1
  emitrust.return
}

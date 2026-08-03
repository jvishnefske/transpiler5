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

// -----

// FR-61c: a while condition op the FR-61d machinery cannot fold (here a
// multi-use value needing a statement binding) is a located error --
// statements cannot render inside a `while` head, and silently wrong
// code is never an option.
emitrust.func @unfoldable_while(%arg0: i32, %arg1: i32) -> i32 {
  %m = emitrust.let mut %arg0 : i32
  emitrust.while {
    // CHECK: condition op does not fold into the head expression
    %t = emitrust.add %m, %arg1 : i32
    %c = emitrust.cmp lt, %t, %t : (i32, i32) -> i1
    emitrust.condition %c
  } do {
    emitrust.yield
  }
  emitrust.return %m : i32
}

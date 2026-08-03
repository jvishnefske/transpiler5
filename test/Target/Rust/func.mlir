// FR-4: Function emission: signature with parameter and return types, body,
// and the FR-61a tail-expression return (a function-final `return;` is
// omitted; `return v;` renders as the tail expression, folding a single-use
// immediately preceding binding).
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK-LABEL: fn empty() {
// CHECK-NEXT:  }
emitrust.func @empty() {
  emitrust.return
}

// CHECK-LABEL: fn answer() -> i32 {
// CHECK-NEXT:    42
// CHECK-NEXT:  }
emitrust.func @answer() -> i32 {
  %0 = emitrust.constant <42 : i32> : i32
  emitrust.return %0 : i32
}

// CHECK-LABEL: fn add_two(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    v0 + v1
// CHECK-NEXT:  }
emitrust.func @add_two(%arg0: i32, %arg1: i32) -> i32 {
  %0 = emitrust.add %arg0, %arg1 : i32
  emitrust.return %0 : i32
}

// CHECK-LABEL: fn pass_through(v0: f64) -> f64 {
// CHECK-NEXT:    v0
// CHECK-NEXT:  }
emitrust.func @pass_through(%arg0: f64) -> f64 {
  emitrust.return %arg0 : f64
}

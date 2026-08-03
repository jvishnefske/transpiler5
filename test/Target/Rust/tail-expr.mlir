// FR-61a: Tail-expression returns. A function-final `return v;` renders as
// the tail expression `v`; a function-final `return;` is omitted; and when
// the returned value is a single-use binding defined by the immediately
// preceding let-producing op, the binding folds away and its right-hand side
// becomes the tail expression. Non-tail returns cannot occur in this dialect:
// `emitrust.return` is a terminator constrained to `emitrust.func`, so every
// representable return already sits in function-final position.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// A plain final `return v;` becomes the tail expression `v`.
// CHECK-LABEL: fn tail_param(v0: i32) -> i32 {
// CHECK-NEXT:    v0
// CHECK-NEXT:  }
emitrust.func @tail_param(%arg0: i32) -> i32 {
  emitrust.return %arg0 : i32
}

// A single-use binding defined by the immediately preceding op folds: the
// `let` never renders and its right-hand side is the tail expression.
// CHECK-LABEL: fn tail_fold(v0: i32) -> i32 {
// CHECK-NEXT:    v0 + v0
// CHECK-NEXT:  }
emitrust.func @tail_fold(%arg0: i32) -> i32 {
  %0 = emitrust.add %arg0, %arg0 : i32
  emitrust.return %0 : i32
}

// The fold applies to constants too.
// CHECK-LABEL: fn tail_fold_constant() -> i32 {
// CHECK-NEXT:    42
// CHECK-NEXT:  }
emitrust.func @tail_fold_constant() -> i32 {
  %0 = emitrust.constant <42 : i32> : i32
  emitrust.return %0 : i32
}

// A multi-use binding does NOT fold (and is not inlined by FR-61d either):
// the `let` stays, later uses keep their names, and only the single-use
// mul folds as the tail expression. The folded functions above and the
// non-folded binding here share the same v-numbering scheme, so folding
// never shifts the numbering of other values.
// CHECK-LABEL: fn tail_multi_use(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: i32 = v0 + v0;
// CHECK-NEXT:    v1 * v1
// CHECK-NEXT:  }
emitrust.func @tail_multi_use(%arg0: i32) -> i32 {
  %0 = emitrust.add %arg0, %arg0 : i32
  %1 = emitrust.mul %0, %0 : i32
  emitrust.return %1 : i32
}

// A function-final `return;` is simply omitted.
// CHECK-LABEL: fn tail_void(v0: i32) {
// CHECK-NEXT:    consume(v0);
// CHECK-NEXT:  }
emitrust.func @tail_void(%arg0: i32) {
  emitrust.call_opaque "consume"(%arg0) : (i32) -> ()
  emitrust.return
}

// A body that is nothing but `return;` renders as an empty body.
// CHECK-LABEL: fn tail_only_return() {
// CHECK-NEXT:  }
emitrust.func @tail_only_return() {
  emitrust.return
}

// A single-use call binding folds: the call expression is the tail.
// CHECK-LABEL: fn tail_fold_call(v0: i32) -> i32 {
// CHECK-NEXT:    produce(v0)
// CHECK-NEXT:  }
emitrust.func @tail_fold_call(%arg0: i32) -> i32 {
  %0 = emitrust.call_opaque "produce"(%arg0) : (i32) -> i32
  emitrust.return %0 : i32
}

// A single-use select folds; Rust allows an `if` expression in tail position.
// CHECK-LABEL: fn tail_fold_select(v0: bool, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    if v0 { v1 } else { v2 }
// CHECK-NEXT:  }
emitrust.func @tail_fold_select(%arg0: i1, %arg1: i32, %arg2: i32) -> i32 {
  %0 = emitrust.select %arg0, %arg1, %arg2 : i32
  emitrust.return %0 : i32
}

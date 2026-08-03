// FR-7: Control-flow emission: if, if-else, for with stepped range, and nesting.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// The unused arm-local constants drop (FR-61d), leaving the pinned
// control-flow skeletons empty.
// CHECK-LABEL: fn simple_if(v0: bool) {
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @simple_if(%arg0: i1) {
  emitrust.if %arg0 {
    %0 = emitrust.constant <1 : i32> : i32
  }
  emitrust.return
}

// CHECK-LABEL: fn if_else(v0: bool) {
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:    } else {
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @if_else(%arg0: i1) {
  emitrust.if %arg0 {
    %0 = emitrust.constant <1 : i32> : i32
  } else {
    %1 = emitrust.constant <2 : i32> : i32
  }
  emitrust.return
}

// The body's unused alias let drops, un-reading the induction variable.
// CHECK-LABEL: fn stepped_loop(v0: usize, v1: usize, v2: usize) {
// CHECK-NEXT:    for _v3 in (v0..v1).step_by(v2 as usize) {
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @stepped_loop(%arg0: index, %arg1: index, %arg2: index) {
  emitrust.for %i = %arg0 to %arg1 step %arg2 {
    %0 = emitrust.let %i : index
  }
  emitrust.return
}

// CHECK-LABEL: fn typed_loop(v0: i32, v1: i32, v2: i32) {
// CHECK-NEXT:    for _v3 in (v0..v1).step_by(v2 as usize) {
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @typed_loop(%arg0: i32, %arg1: i32, %arg2: i32) {
  emitrust.for %i = %arg0 to %arg1 step %arg2 : i32 {
    %0 = emitrust.add %i, %i : i32
  }
  emitrust.return
}

// FR-61b: a deferred binding immediately followed by an if whose BOTH arms
// end with the binding's only two assignments renders as an if-expression
// binding: each arm's final assignment becomes the arm's tail expression.
// FR-61d slice 3: when the binding's only read is the function-final
// return, the `let` folds away and the if-expression IS the tail.
// CHECK-LABEL: fn if_expr_binding(v0: bool, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:      v1
// CHECK-NEXT:    } else {
// CHECK-NEXT:      v2
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @if_expr_binding(%arg0: i1, %arg1: i32, %arg2: i32) -> i32 {
  %0 = emitrust.let mut %arg1 : i32
  emitrust.if %arg0 {
    emitrust.assign %0 = %arg1 : i32
  } else {
    emitrust.assign %0 = %arg2 : i32
  }
  emitrust.return %0 : i32
}

// FR-61b: arm statements before the final assignment stay as statements;
// the final assignment turns into the arm's tail expression -- and with
// FR-61d slice 3's capture routing the arm-local single-use producers
// inline straight into that tail.
// CHECK-LABEL: fn if_expr_binding_stmts(v0: bool, v1: i32) -> i32 {
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:      v1 + v1
// CHECK-NEXT:    } else {
// CHECK-NEXT:      0i32
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @if_expr_binding_stmts(%arg0: i1, %arg1: i32) -> i32 {
  %0 = emitrust.let mut %arg1 : i32
  emitrust.if %arg0 {
    %1 = emitrust.add %arg1, %arg1 : i32
    emitrust.assign %0 = %1 : i32
  } else {
    %2 = emitrust.constant <0 : i32> : i32
    emitrust.assign %0 = %2 : i32
  }
  emitrust.return %0 : i32
}

// FR-61b out of scope: a deferred binding that still needs `mut` (an arm
// assigns it twice) keeps today's statement rendering.
// CHECK-LABEL: fn if_stmt_mut(v0: bool, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    let mut v3: i32;
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:      v3 = v1;
// CHECK-NEXT:      v3 = v2;
// CHECK-NEXT:    } else {
// CHECK-NEXT:      v3 = v2;
// CHECK-NEXT:    }
// CHECK-NEXT:    v3
// CHECK-NEXT:  }
emitrust.func @if_stmt_mut(%arg0: i1, %arg1: i32, %arg2: i32) -> i32 {
  %0 = emitrust.let mut %arg1 : i32
  emitrust.if %arg0 {
    emitrust.assign %0 = %arg1 : i32
    emitrust.assign %0 = %arg2 : i32
  } else {
    emitrust.assign %0 = %arg2 : i32
  }
  emitrust.return %0 : i32
}

// FR-61b out of scope: a one-arm assignment leaves the initializer live, so
// the binding keeps its `let mut` statement form.
// CHECK-LABEL: fn one_arm(v0: bool, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    let mut v3: i32 = v1;
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:      v3 = v2;
// CHECK-NEXT:    }
// CHECK-NEXT:    v3
// CHECK-NEXT:  }
emitrust.func @one_arm(%arg0: i1, %arg1: i32, %arg2: i32) -> i32 {
  %0 = emitrust.let mut %arg1 : i32
  emitrust.if %arg0 {
    emitrust.assign %0 = %arg2 : i32
  }
  emitrust.return %0 : i32
}

// CHECK-LABEL: fn nested(v0: usize, v1: usize, v2: usize, v3: bool) {
// CHECK-NEXT:    for _v4 in (v0..v1).step_by(v2 as usize) {
// CHECK-NEXT:      if v3 {
// CHECK-NEXT:      }
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @nested(%arg0: index, %arg1: index, %arg2: index, %arg3: i1) {
  emitrust.for %i = %arg0 to %arg1 step %arg2 {
    emitrust.if %arg3 {
      %0 = emitrust.let %i : index
    }
  }
  emitrust.return
}

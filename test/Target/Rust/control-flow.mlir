// FR-7: Control-flow emission: if, if-else, for with stepped range, and nesting.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK-LABEL: fn simple_if(v0: bool) {
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:      let v1: i32 = 1;
// CHECK-NEXT:    }
// CHECK-NEXT:    return;
// CHECK-NEXT:  }
emitrust.func @simple_if(%arg0: i1) {
  emitrust.if %arg0 {
    %0 = emitrust.constant <1 : i32> : i32
  }
  emitrust.return
}

// CHECK-LABEL: fn if_else(v0: bool) {
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:      let v1: i32 = 1;
// CHECK-NEXT:    } else {
// CHECK-NEXT:      let v2: i32 = 2;
// CHECK-NEXT:    }
// CHECK-NEXT:    return;
// CHECK-NEXT:  }
emitrust.func @if_else(%arg0: i1) {
  emitrust.if %arg0 {
    %0 = emitrust.constant <1 : i32> : i32
  } else {
    %1 = emitrust.constant <2 : i32> : i32
  }
  emitrust.return
}

// CHECK-LABEL: fn stepped_loop(v0: usize, v1: usize, v2: usize) {
// CHECK-NEXT:    for v3 in (v0..v1).step_by(v2 as usize) {
// CHECK-NEXT:      let v4: usize = v3;
// CHECK-NEXT:    }
// CHECK-NEXT:    return;
// CHECK-NEXT:  }
emitrust.func @stepped_loop(%arg0: index, %arg1: index, %arg2: index) {
  emitrust.for %i = %arg0 to %arg1 step %arg2 {
    %0 = emitrust.let %i : index
  }
  emitrust.return
}

// CHECK-LABEL: fn typed_loop(v0: i32, v1: i32, v2: i32) {
// CHECK-NEXT:    for v3 in (v0..v1).step_by(v2 as usize) {
// CHECK-NEXT:      let v4: i32 = v3 + v3;
// CHECK-NEXT:    }
// CHECK-NEXT:    return;
// CHECK-NEXT:  }
emitrust.func @typed_loop(%arg0: i32, %arg1: i32, %arg2: i32) {
  emitrust.for %i = %arg0 to %arg1 step %arg2 : i32 {
    %0 = emitrust.add %i, %i : i32
  }
  emitrust.return
}

// CHECK-LABEL: fn nested(v0: usize, v1: usize, v2: usize, v3: bool) {
// CHECK-NEXT:    for v4 in (v0..v1).step_by(v2 as usize) {
// CHECK-NEXT:      if v3 {
// CHECK-NEXT:        let v5: usize = v4;
// CHECK-NEXT:      }
// CHECK-NEXT:    }
// CHECK-NEXT:    return;
// CHECK-NEXT:  }
emitrust.func @nested(%arg0: index, %arg1: index, %arg2: index, %arg3: i1) {
  emitrust.for %i = %arg0 to %arg1 step %arg2 {
    emitrust.if %arg3 {
      %0 = emitrust.let %i : index
    }
  }
  emitrust.return
}

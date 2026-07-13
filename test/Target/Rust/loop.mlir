// M1: loop emission with conditional break and continue, and loop jumps
// inside a for loop.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK-LABEL: fn spin(v0: bool, v1: bool) {
// CHECK-NEXT:    loop {
// CHECK-NEXT:      if v0 {
// CHECK-NEXT:        break;
// CHECK-NEXT:      }
// CHECK-NEXT:      if v1 {
// CHECK-NEXT:        continue;
// CHECK-NEXT:      }
// CHECK-NEXT:    }
// CHECK-NEXT:    return;
// CHECK-NEXT:  }
emitrust.func @spin(%arg0: i1, %arg1: i1) {
  emitrust.loop {
    emitrust.if %arg0 {
      emitrust.break
    }
    emitrust.if %arg1 {
      emitrust.continue
    }
  }
  emitrust.return
}

// CHECK-LABEL: fn for_jumps(v0: usize, v1: usize, v2: usize, v3: bool) {
// CHECK-NEXT:    for v4 in (v0..v1).step_by(v2 as usize) {
// CHECK-NEXT:      if v3 {
// CHECK-NEXT:        continue;
// CHECK-NEXT:      }
// CHECK-NEXT:      break;
// CHECK-NEXT:    }
// CHECK-NEXT:    return;
// CHECK-NEXT:  }
emitrust.func @for_jumps(%arg0: index, %arg1: index, %arg2: index, %arg3: i1) {
  emitrust.for %i = %arg0 to %arg1 step %arg2 {
    emitrust.if %arg3 {
      emitrust.continue
    }
    emitrust.break
  }
  emitrust.return
}

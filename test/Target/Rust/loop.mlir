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
// CHECK-NEXT:    for _v4 in (v0..v1).step_by(v2 as usize) {
// CHECK-NEXT:      if v3 {
// CHECK-NEXT:        continue;
// CHECK-NEXT:      }
// CHECK-NEXT:      break;
// CHECK-NEXT:    }
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

// FR-25: the staged parallel-assignment shape produced for copy-cycle loop
// back-edges (lost-copy problem) renders as immutable temporary lets that
// capture every source before any carried variable is assigned.
// CHECK-LABEL: fn rotate(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    let mut v2: i32 = v0;
// CHECK-NEXT:    let mut v3: i32 = v1;
// CHECK-NEXT:    loop {
// CHECK-NEXT:      let v4: i32 = v3;
// CHECK-NEXT:      let v5: i32 = v2;
// CHECK-NEXT:      v2 = v4;
// CHECK-NEXT:      v3 = v5;
// CHECK-NEXT:      break;
// CHECK-NEXT:    }
// CHECK-NEXT:    v2
// CHECK-NEXT:  }
emitrust.func @rotate(%arg0: i32, %arg1: i32) -> i32 {
  %a = emitrust.let mut %arg0 : i32
  %b = emitrust.let mut %arg1 : i32
  emitrust.loop {
    %ta = emitrust.let %b : i32
    %tb = emitrust.let %a : i32
    emitrust.assign %a = %ta : i32
    emitrust.assign %b = %tb : i32
    emitrust.break
  }
  emitrust.return %a : i32
}

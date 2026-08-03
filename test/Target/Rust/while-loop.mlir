// FR-61c: `emitrust.while` renders as a Rust `while` whose head is the
// condition region folded through the FR-61d capture machinery -- the
// pure single-use chain inlines into ONE expression in the
// never-parenthesized condition position, and every condition op must be
// consumed (a leftover statement is a located error, never wrong code).
// Carried values stay `let mut` bindings above the loop with backedge
// assignments at the body's end.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// A carried counter: the condition chain (constant + cmp) folds into the
// head; the body keeps its statements and the backedge assignment.
// CHECK-LABEL: fn count(v0: i32) -> i32 {
// CHECK-NEXT:    let mut v2: i32 = 0i32;
// CHECK-NEXT:    while v2 < v0 {
// CHECK-NEXT:        v2 = v2 + 1i32;
// CHECK-NEXT:    }
// CHECK-NEXT:    v2
// CHECK-NEXT:  }
emitrust.func @count(%arg0: i32) -> i32 {
  %zero = emitrust.constant <0 : i32> : i32
  %i = emitrust.let mut %zero : i32
  emitrust.while {
    %c = emitrust.cmp lt, %i, %arg0 : (i32, i32) -> i1
    emitrust.condition %c
  } do {
    %one = emitrust.constant <1 : i32> : i32
    %next = emitrust.add %i, %one : i32
    emitrust.assign %i = %next : i32
    emitrust.yield
  }
  emitrust.return %i : i32
}

// A multi-op condition chain folds into one head expression: the masked
// read inlines into the comparison bare (Rust's `&` binds tighter than
// `!=`, unlike C), the comparison into the head.
// CHECK-LABEL: fn masked(v0: i32, _v1: i32) -> i32 {
// CHECK-NEXT:    let mut v2: i32 = v0;
// CHECK-NEXT:    while v2 & 7i32 != 0i32 {
// CHECK-NEXT:        v2 = v2 >> 1i32;
// CHECK-NEXT:    }
// CHECK-NEXT:    v2
// CHECK-NEXT:  }
emitrust.func @masked(%arg0: i32, %arg1: i32) -> i32 {
  %m = emitrust.let mut %arg0 : i32
  emitrust.while {
    %mask = emitrust.constant <7 : i32> : i32
    %bits = emitrust.and %m, %mask : i32
    %zero = emitrust.constant <0 : i32> : i32
    %c = emitrust.cmp ne, %bits, %zero : (i32, i32) -> i1
    emitrust.condition %c
  } do {
    %one = emitrust.constant <1 : i32> : i32
    %next = emitrust.shr %m, %one : i32
    emitrust.assign %m = %next : i32
    emitrust.yield
  }
  emitrust.return %m : i32
}

// `while true`: an inlined bool constant head (C's `while(1)` with the
// exit elsewhere in the body).
// CHECK-LABEL: fn spin_forever(v0: bool) {
// CHECK-NEXT:    while true {
// CHECK-NEXT:        if v0 {
// CHECK-NEXT:            break;
// CHECK-NEXT:        }
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @spin_forever(%arg0: i1) {
  emitrust.while {
    %t = emitrust.constant <true> : i1
    emitrust.condition %t
  } do {
    emitrust.if %arg0 {
      emitrust.break
    }
    emitrust.yield
  }
  emitrust.return
}

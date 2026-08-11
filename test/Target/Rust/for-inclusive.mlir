// FR-61f widening: an `emitrust.for` carrying the `inclusive` unit attribute
// renders Rust's inclusive range `lo..=hi` — the image of C's `i <= HI`
// ascending counting loop — while the attribute's absence keeps the
// historical half-open `lo..hi`. Pins both step forms: a unit step drops
// `.step_by` with no wrapping parens, a non-unit step keeps
// `(lo..=hi).step_by(k as usize)`.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK-LABEL: fn for_inclusive(v0: i32) {
// CHECK-NEXT:    for _v3 in 1i32..=v0 {
// CHECK-NEXT:        body();
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @for_inclusive(%arg0: i32) {
  %lo = emitrust.constant <1 : i32> : i32
  %one = emitrust.constant <1 : i32> : i32
  emitrust.for %i = %lo to %arg0 step %one : i32 {
    emitrust.call_opaque "body"() : () -> ()
  } {inclusive}
  emitrust.return
}

// CHECK-LABEL: fn for_inclusive_step(v0: i32) {
// CHECK-NEXT:    for _v3 in (0i32..=v0).step_by(2i32 as usize) {
// CHECK-NEXT:        body();
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @for_inclusive_step(%arg0: i32) {
  %lo = emitrust.constant <0 : i32> : i32
  %two = emitrust.constant <2 : i32> : i32
  emitrust.for %i = %lo to %arg0 step %two : i32 {
    emitrust.call_opaque "body"() : () -> ()
  } {inclusive}
  emitrust.return
}

// The attribute's absence keeps the half-open range (no golden shift for
// every existing exclusive loop).
// CHECK-LABEL: fn for_exclusive(v0: i32) {
// CHECK-NEXT:    for _v3 in 0i32..v0 {
emitrust.func @for_exclusive(%arg0: i32) {
  %lo = emitrust.constant <0 : i32> : i32
  %one = emitrust.constant <1 : i32> : i32
  emitrust.for %i = %lo to %arg0 step %one : i32 {
    emitrust.call_opaque "body"() : () -> ()
  }
  emitrust.return
}

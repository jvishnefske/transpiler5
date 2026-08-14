// FR-69: non-finite float constant emission. A NaN FloatAttr renders as
// `fW::from_bits(0x<BITS>uW)` with the exact APFloat bit pattern — Rust's
// f32/f64::NAN pins neither sign nor payload, and the importer's %f/%g
// shims print nan/-nan by sign bit, so the bits (not a NAN path constant)
// are the only byte-exact spelling. Infinities keep their deterministic
// `::INFINITY`/`::NEG_INFINITY` path spelling (first coverage of that
// arm). Non-finite constants never inline (no literal suffix spelling),
// so every one is pinned as a let binding; a finite constant in the same
// call still inlines with its suffix, pinning that emitFloatValue is
// untouched. A NaN in an `emitrust.global` initializer renders inside the
// FR-63 `const { ... }` block: from_bits is a const fn on the pinned
// toolchain.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK:      thread_local! {
// CHECK-NEXT:     static gnan: std::cell::Cell<f64> = const { std::cell::Cell::new(f64::from_bits(0x7FF8000000000000u64)) };
// CHECK-NEXT: }
emitrust.global @gnan <0x7FF8000000000000 : f64> : f64

// CHECK-LABEL: fn nans() {
// CHECK-NEXT:    let v0: f64 = f64::from_bits(0x7FF8000000000000u64);
// CHECK-NEXT:    let v1: f64 = f64::from_bits(0xFFF8000000000000u64);
// CHECK-NEXT:    let v2: f32 = f32::from_bits(0x7FC00000u32);
// CHECK-NEXT:    let v3: f32 = f32::from_bits(0xFFC00000u32);
// CHECK-NEXT:    sink(v0, v1, v2, v3, 4.2f64);
emitrust.func @nans() {
  %0 = emitrust.constant <0x7FF8000000000000 : f64> : f64
  %1 = emitrust.constant <0xFFF8000000000000 : f64> : f64
  %2 = emitrust.constant <0x7FC00000 : f32> : f32
  %3 = emitrust.constant <0xFFC00000 : f32> : f32
  %4 = emitrust.constant <4.2 : f64> : f64
  emitrust.call_opaque "sink"(%0, %1, %2, %3, %4)
      : (f64, f64, f32, f32, f64) -> ()
  emitrust.return
}

// CHECK-LABEL: fn infinities() {
// CHECK-NEXT:    let v0: f64 = f64::INFINITY;
// CHECK-NEXT:    let v1: f64 = f64::NEG_INFINITY;
// CHECK-NEXT:    let v2: f32 = f32::INFINITY;
// CHECK-NEXT:    let v3: f32 = f32::NEG_INFINITY;
// CHECK-NEXT:    sink(v0, v1, v2, v3);
emitrust.func @infinities() {
  %0 = emitrust.constant <0x7FF0000000000000 : f64> : f64
  %1 = emitrust.constant <0xFFF0000000000000 : f64> : f64
  %2 = emitrust.constant <0x7F800000 : f32> : f32
  %3 = emitrust.constant <0xFF800000 : f32> : f32
  emitrust.call_opaque "sink"(%0, %1, %2, %3)
      : (f64, f64, f32, f32) -> ()
  emitrust.return
}

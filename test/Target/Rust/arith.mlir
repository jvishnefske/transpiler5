// FR-5: Expression emission: constant, literal, call_opaque,
// add/sub/mul/div/rem, and/or/xor/shl/shr, cmp, cast; unsigned add/sub/mul
// render as the wrapping_* method calls (C99-2 wrap-around semantics).
// Since FR-61d, a single-use pure value renders inline at its consumer (an
// unused one emits nothing at all), so each family feeds a sink call and
// the operator renderings are pinned in the call's argument list.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK-LABEL: fn binops(v0: i32, v1: i32) {
// CHECK-NEXT:    sink(v0 + v1, v0 - v1, v0 * v1, v0 / v1, v0 % v1);
emitrust.func @binops(%arg0: i32, %arg1: i32) {
  %0 = emitrust.add %arg0, %arg1 : i32
  %1 = emitrust.sub %arg0, %arg1 : i32
  %2 = emitrust.mul %arg0, %arg1 : i32
  %3 = emitrust.div %arg0, %arg1 : i32
  %4 = emitrust.rem %arg0, %arg1 : i32
  emitrust.call_opaque "sink"(%0, %1, %2, %3, %4)
      : (i32, i32, i32, i32, i32) -> ()
  emitrust.return
}

// CHECK-LABEL: fn bitops(v0: i32, v1: i32) {
// CHECK-NEXT:    sink(v0 & v1, v0 | v1, v0 ^ v1, v0 << v1, v0 >> v1);
emitrust.func @bitops(%arg0: i32, %arg1: i32) {
  %0 = emitrust.and %arg0, %arg1 : i32
  %1 = emitrust.or %arg0, %arg1 : i32
  %2 = emitrust.xor %arg0, %arg1 : i32
  %3 = emitrust.shl %arg0, %arg1 : i32
  %4 = emitrust.shr %arg0, %arg1 : i32
  emitrust.call_opaque "sink"(%0, %1, %2, %3, %4)
      : (i32, i32, i32, i32, i32) -> ()
  emitrust.return
}

// Unsigned add/sub/mul render as the wrapping_* method calls: C defines
// unsigned overflow as wrap-around, whereas Rust's infix operators panic
// in debug builds. Division, remainder, bitwise, shift, and comparison
// keep the infix form — Rust's operators on uN already carry the unsigned
// semantics (`>>` on u32 is the logical shift).
// CHECK-LABEL: fn unsigned_binops(v0: u32, v1: u32) {
// CHECK-NEXT:    sink(v0.wrapping_add(v1), v0.wrapping_sub(v1), v0.wrapping_mul(v1), v0 / v1, v0 % v1, v0 & v1, v0 << v1, v0 >> v1, v0 < v1);
emitrust.func @unsigned_binops(%arg0: ui32, %arg1: ui32) {
  %0 = emitrust.add %arg0, %arg1 : ui32
  %1 = emitrust.sub %arg0, %arg1 : ui32
  %2 = emitrust.mul %arg0, %arg1 : ui32
  %3 = emitrust.div %arg0, %arg1 : ui32
  %4 = emitrust.rem %arg0, %arg1 : ui32
  %5 = emitrust.and %arg0, %arg1 : ui32
  %6 = emitrust.shl %arg0, %arg1 : ui32
  %7 = emitrust.shr %arg0, %arg1 : ui32
  %8 = emitrust.cmp lt, %arg0, %arg1 : (ui32, ui32) -> i1
  emitrust.call_opaque "sink"(%0, %1, %2, %3, %4, %5, %6, %7, %8)
      : (ui32, ui32, ui32, ui32, ui32, ui32, ui32, ui32, i1) -> ()
  emitrust.return
}

// The wrapping form follows the result type across all unsigned widths.
// CHECK-LABEL: fn unsigned_widths(v0: u8, v1: u64) {
// CHECK-NEXT:    sink(v0.wrapping_add(v0), v1.wrapping_mul(v1));
emitrust.func @unsigned_widths(%arg0: ui8, %arg1: ui64) {
  %0 = emitrust.add %arg0, %arg0 : ui8
  %1 = emitrust.mul %arg1, %arg1 : ui64
  emitrust.call_opaque "sink"(%0, %1) : (ui8, ui64) -> ()
  emitrust.return
}

// CHECK-LABEL: fn compares(v0: i32, v1: i32) {
// CHECK-NEXT:    sink(v0 == v1, v0 != v1, v0 < v1, v0 <= v1, v0 > v1, v0 >= v1);
emitrust.func @compares(%arg0: i32, %arg1: i32) {
  %0 = emitrust.cmp eq, %arg0, %arg1 : (i32, i32) -> i1
  %1 = emitrust.cmp ne, %arg0, %arg1 : (i32, i32) -> i1
  %2 = emitrust.cmp lt, %arg0, %arg1 : (i32, i32) -> i1
  %3 = emitrust.cmp le, %arg0, %arg1 : (i32, i32) -> i1
  %4 = emitrust.cmp gt, %arg0, %arg1 : (i32, i32) -> i1
  %5 = emitrust.cmp ge, %arg0, %arg1 : (i32, i32) -> i1
  emitrust.call_opaque "sink"(%0, %1, %2, %3, %4, %5)
      : (i1, i1, i1, i1, i1, i1) -> ()
  emitrust.return
}

// CHECK-LABEL: fn casts(v0: i32) {
// CHECK-NEXT:    sink(v0 as i64, v0 as f64);
emitrust.func @casts(%arg0: i32) {
  %0 = emitrust.cast %arg0 : i32 to i64
  %1 = emitrust.cast %arg0 : i32 to f64
  emitrust.call_opaque "sink"(%0, %1) : (i64, f64) -> ()
  emitrust.return
}

// Bit-exact float/integer reinterpretation: unsigned sides speak
// to_bits/from_bits directly; signless sides wrap the same-width
// (bit-preserving) `as` conversion the u32/u64 bits type requires.
// CHECK-LABEL: fn bitcasts(v0: f32, v1: u32, v2: i64, v3: f64) {
// CHECK-NEXT:    sink(v0.to_bits(), v0.to_bits() as i32, f32::from_bits(v1), f64::from_bits(v2 as u64), v3.to_bits());
emitrust.func @bitcasts(%arg0: f32, %arg1: ui32, %arg2: i64, %arg3: f64) {
  %0 = emitrust.bitcast %arg0 : f32 to ui32
  %1 = emitrust.bitcast %arg0 : f32 to i32
  %2 = emitrust.bitcast %arg1 : ui32 to f32
  %3 = emitrust.bitcast %arg2 : i64 to f64
  %4 = emitrust.bitcast %arg3 : f64 to ui64
  emitrust.call_opaque "sink"(%0, %1, %2, %3, %4)
      : (ui32, i32, f32, f64, ui64) -> ()
  emitrust.return
}

// Inlined numeric constants always carry their literal type suffix; bool
// and opaque constants render as-is.
// CHECK-LABEL: fn constants() {
// CHECK-NEXT:    sink(42i32, 4.2f64, 1.0f32, true, Vec::new());
emitrust.func @constants() {
  %0 = emitrust.constant <42 : i32> : i32
  %1 = emitrust.constant <4.2 : f64> : f64
  %2 = emitrust.constant <1.0 : f32> : f32
  %3 = emitrust.constant <true> : i1
  %4 = emitrust.constant <#emitrust.opaque<"Vec::new()">> : !emitrust.opaque<"Vec<i32>">
  emitrust.call_opaque "sink"(%0, %1, %2, %3, %4)
      : (i32, f64, f32, i1, !emitrust.opaque<"Vec<i32>">) -> ()
  emitrust.return
}

// A literal is neither inlined nor dropped: its text is opaque, so it
// keeps its (underscore-named, here unused) binding.
// CHECK-LABEL: fn literals() {
// CHECK-NEXT:    let _v0: usize = x.len();
emitrust.func @literals() {
  %0 = emitrust.literal "x.len()" : index
  emitrust.return
}

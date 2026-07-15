// FR-5: Expression emission: constant, literal, call_opaque,
// add/sub/mul/div/rem, and/or/xor/shl/shr, cmp, cast; unsigned add/sub/mul
// render as the wrapping_* method calls (C99-2 wrap-around semantics).
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK-LABEL: fn binops(v0: i32, v1: i32) {
// CHECK-NEXT:    let v2: i32 = v0 + v1;
// CHECK-NEXT:    let v3: i32 = v0 - v1;
// CHECK-NEXT:    let v4: i32 = v0 * v1;
// CHECK-NEXT:    let v5: i32 = v0 / v1;
// CHECK-NEXT:    let v6: i32 = v0 % v1;
emitrust.func @binops(%arg0: i32, %arg1: i32) {
  %0 = emitrust.add %arg0, %arg1 : i32
  %1 = emitrust.sub %arg0, %arg1 : i32
  %2 = emitrust.mul %arg0, %arg1 : i32
  %3 = emitrust.div %arg0, %arg1 : i32
  %4 = emitrust.rem %arg0, %arg1 : i32
  emitrust.return
}

// CHECK-LABEL: fn bitops(v0: i32, v1: i32) {
// CHECK-NEXT:    let v2: i32 = v0 & v1;
// CHECK-NEXT:    let v3: i32 = v0 | v1;
// CHECK-NEXT:    let v4: i32 = v0 ^ v1;
// CHECK-NEXT:    let v5: i32 = v0 << v1;
// CHECK-NEXT:    let v6: i32 = v0 >> v1;
emitrust.func @bitops(%arg0: i32, %arg1: i32) {
  %0 = emitrust.and %arg0, %arg1 : i32
  %1 = emitrust.or %arg0, %arg1 : i32
  %2 = emitrust.xor %arg0, %arg1 : i32
  %3 = emitrust.shl %arg0, %arg1 : i32
  %4 = emitrust.shr %arg0, %arg1 : i32
  emitrust.return
}

// Unsigned add/sub/mul render as the wrapping_* method calls: C defines
// unsigned overflow as wrap-around, whereas Rust's infix operators panic
// in debug builds. Division, remainder, bitwise, shift, and comparison
// keep the infix form — Rust's operators on uN already carry the unsigned
// semantics (`>>` on u32 is the logical shift).
// CHECK-LABEL: fn unsigned_binops(v0: u32, v1: u32) {
// CHECK-NEXT:    let v2: u32 = v0.wrapping_add(v1);
// CHECK-NEXT:    let v3: u32 = v0.wrapping_sub(v1);
// CHECK-NEXT:    let v4: u32 = v0.wrapping_mul(v1);
// CHECK-NEXT:    let v5: u32 = v0 / v1;
// CHECK-NEXT:    let v6: u32 = v0 % v1;
// CHECK-NEXT:    let v7: u32 = v0 & v1;
// CHECK-NEXT:    let v8: u32 = v0 << v1;
// CHECK-NEXT:    let v9: u32 = v0 >> v1;
// CHECK-NEXT:    let v10: bool = v0 < v1;
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
  emitrust.return
}

// The wrapping form follows the result type across all unsigned widths.
// CHECK-LABEL: fn unsigned_widths(v0: u8, v1: u64) {
// CHECK-NEXT:    let v2: u8 = v0.wrapping_add(v0);
// CHECK-NEXT:    let v3: u64 = v1.wrapping_mul(v1);
emitrust.func @unsigned_widths(%arg0: ui8, %arg1: ui64) {
  %0 = emitrust.add %arg0, %arg0 : ui8
  %1 = emitrust.mul %arg1, %arg1 : ui64
  emitrust.return
}

// CHECK-LABEL: fn compares(v0: i32, v1: i32) {
// CHECK-NEXT:    let v2: bool = v0 == v1;
// CHECK-NEXT:    let v3: bool = v0 != v1;
// CHECK-NEXT:    let v4: bool = v0 < v1;
// CHECK-NEXT:    let v5: bool = v0 <= v1;
// CHECK-NEXT:    let v6: bool = v0 > v1;
// CHECK-NEXT:    let v7: bool = v0 >= v1;
emitrust.func @compares(%arg0: i32, %arg1: i32) {
  %0 = emitrust.cmp eq, %arg0, %arg1 : (i32, i32) -> i1
  %1 = emitrust.cmp ne, %arg0, %arg1 : (i32, i32) -> i1
  %2 = emitrust.cmp lt, %arg0, %arg1 : (i32, i32) -> i1
  %3 = emitrust.cmp le, %arg0, %arg1 : (i32, i32) -> i1
  %4 = emitrust.cmp gt, %arg0, %arg1 : (i32, i32) -> i1
  %5 = emitrust.cmp ge, %arg0, %arg1 : (i32, i32) -> i1
  emitrust.return
}

// CHECK-LABEL: fn casts(v0: i32) {
// CHECK-NEXT:    let v1: i64 = v0 as i64;
// CHECK-NEXT:    let v2: f64 = v0 as f64;
emitrust.func @casts(%arg0: i32) {
  %0 = emitrust.cast %arg0 : i32 to i64
  %1 = emitrust.cast %arg0 : i32 to f64
  emitrust.return
}

// CHECK-LABEL: fn constants() {
// CHECK-NEXT:    let v0: i32 = 42;
// CHECK-NEXT:    let v1: f64 = 4.2;
// CHECK-NEXT:    let v2: f32 = 1.0;
// CHECK-NEXT:    let v3: bool = true;
// CHECK-NEXT:    let v4: Vec<i32> = Vec::new();
emitrust.func @constants() {
  %0 = emitrust.constant <42 : i32> : i32
  %1 = emitrust.constant <4.2 : f64> : f64
  %2 = emitrust.constant <1.0 : f32> : f32
  %3 = emitrust.constant <true> : i1
  %4 = emitrust.constant <#emitrust.opaque<"Vec::new()">> : !emitrust.opaque<"Vec<i32>">
  emitrust.return
}

// CHECK-LABEL: fn literals() {
// CHECK-NEXT:    let v0: usize = x.len();
emitrust.func @literals() {
  %0 = emitrust.literal "x.len()" : index
  emitrust.return
}

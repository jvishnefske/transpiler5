// CTS-P10: Rust rendering of the cell-slice surface. A
// `!emitrust.ref<!emitrust.cell_slice<T>>` parameter renders as
// `&[std::cell::Cell<T>]`; `emitrust.cell_get`/`emitrust.cell_set`
// render as `slice[idx as usize].get()` / `.set(v)` (the `as usize`
// cast follows the slice-subscript convention); and
// `emitrust.global_cells` renders as the global's `.with` accessor
// binding `__emitrust_tl`, whose `[T; N]` Cell is flattened with
// `.as_slice_of_cells()` into the region's `&[std::cell::Cell<T>]`
// value. Nested regions render as nested `.with` closures, so no borrow
// ever escapes its accessor.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

emitrust.global @g1 : !emitrust.array<4xi32>
emitrust.global @g2 : !emitrust.array<4xi32>

// CHECK: fn observe(v0: &[std::cell::Cell<i32>], v1: i64) -> i32 {
emitrust.func @observe(%arg0: !emitrust.ref<!emitrust.cell_slice<i32>>, %arg1: i64) -> i32 {
  // CHECK: let v2: i32 = v0[v1 as usize].get();
  %v = emitrust.cell_get %arg0[%arg1] : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64) -> i32
  // CHECK: v0[v1 as usize].set(v2);
  emitrust.cell_set %arg0[%arg1], %v : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64, i32) -> ()
  // CHECK: return v2;
  emitrust.return %v : i32
}

// A byte cell-slice renders with the u8-facing element type i8.
// CHECK: fn bytes(_v0: &[std::cell::Cell<i8>]) {
emitrust.func @bytes(%arg0: !emitrust.ref<!emitrust.cell_slice<i8>>) {
  emitrust.return
}

// CHECK: fn caller() {
emitrust.func @caller() {
  // CHECK: g1.with(|__emitrust_tl| {
  // CHECK: &[std::cell::Cell<i32>] = __emitrust_tl.as_slice_of_cells();
  emitrust.global_cells @g1 {
  ^bb0(%c1: !emitrust.ref<!emitrust.cell_slice<i32>>):
    // The nested second global opens its own .with closure inside.
    // CHECK: g2.with(|__emitrust_tl| {
    // CHECK: &[std::cell::Cell<i32>] = __emitrust_tl.as_slice_of_cells();
    emitrust.global_cells @g2 {
    ^bb0(%c2: !emitrust.ref<!emitrust.cell_slice<i32>>):
      %i = emitrust.constant <1 : i64> : i64
      // CHECK: observe(
      %r = emitrust.call_opaque "observe"(%c1, %i) : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64) -> i32
      // CHECK: .set(
      emitrust.cell_set %c2[%i], %r : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64, i32) -> ()
      emitrust.yield
    }
    emitrust.yield
  }
  // CHECK: return;
  emitrust.return
}

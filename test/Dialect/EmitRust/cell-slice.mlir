// CTS-P10: parse/print round-trip of the cell-slice surface. The
// `!emitrust.cell_slice<T>` type is a dynamically sized run of
// `Cell<T>` elements; like `!emitrust.slice` it is unsized and only
// valid behind a reference (always the shared `!emitrust.ref` — Cells
// give interior mutability, so no mut_ref form exists). Element
// accesses (`emitrust.cell_get`/`emitrust.cell_set`) operate on the
// reference directly at an i64 index; `emitrust.global_cells` borrows a
// mutable global's cell-slice for the extent of its region, whose entry
// block argument is the borrowed `!emitrust.ref<!emitrust.cell_slice<T>>`.
// RUN: emitrust-opt %s | emitrust-opt | FileCheck %s

// CHECK-LABEL: emitrust.func @cell_slice_param(
// CHECK-SAME: !emitrust.ref<!emitrust.cell_slice<i32>>
emitrust.func @cell_slice_param(%arg0: !emitrust.ref<!emitrust.cell_slice<i32>>) {
  emitrust.return
}

// A byte cell-slice (the global char-array case).
// CHECK-LABEL: emitrust.func @cell_slice_byte(
// CHECK-SAME: !emitrust.ref<!emitrust.cell_slice<i8>>
emitrust.func @cell_slice_byte(%arg0: !emitrust.ref<!emitrust.cell_slice<i8>>) {
  emitrust.return
}

// Element access round-trip: get at an i64 index, set of a value of the
// element type.
// CHECK-LABEL: emitrust.func @cell_access
emitrust.func @cell_access(%arg0: !emitrust.ref<!emitrust.cell_slice<i32>>, %arg1: i64) {
  // CHECK: %[[V:.*]] = emitrust.cell_get %{{.*}}[%{{.*}}] : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64) -> i32
  %v = emitrust.cell_get %arg0[%arg1] : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64) -> i32
  // CHECK: emitrust.cell_set %{{.*}}[%{{.*}}], %[[V]] : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64, i32) -> ()
  emitrust.cell_set %arg0[%arg1], %v : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64, i32) -> ()
  emitrust.return
}

emitrust.global @g1 : !emitrust.array<4xi32>
emitrust.global @g2 : !emitrust.array<4xi32>

// A cell-slice parameter forwards freely (shared references are
// duplicable): both as a call operand and into a nested call.
// CHECK-LABEL: emitrust.func @forward
emitrust.func @forward(%arg0: !emitrust.ref<!emitrust.cell_slice<i32>>) {
  // CHECK: emitrust.call_opaque "observe"(%{{.*}}) : (!emitrust.ref<!emitrust.cell_slice<i32>>) -> ()
  emitrust.call_opaque "observe"(%arg0) : (!emitrust.ref<!emitrust.cell_slice<i32>>) -> ()
  emitrust.return
}

// global_cells round-trip: the region's entry block argument is the
// borrowed cell-slice; regions nest for calls taking several globals.
// CHECK-LABEL: emitrust.func @with_cells
emitrust.func @with_cells() {
  // CHECK: emitrust.global_cells @g1
  // CHECK: !emitrust.ref<!emitrust.cell_slice<i32>>
  emitrust.global_cells @g1 {
  ^bb0(%cells: !emitrust.ref<!emitrust.cell_slice<i32>>):
    %i = emitrust.constant <0 : i64> : i64
    // CHECK: emitrust.cell_get
    %v = emitrust.cell_get %cells[%i] : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64) -> i32
    // CHECK: emitrust.cell_set
    emitrust.cell_set %cells[%i], %v : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64, i32) -> ()
    emitrust.yield
  }
  // CHECK: emitrust.global_cells @g1
  emitrust.global_cells @g1 {
  ^bb0(%c1: !emitrust.ref<!emitrust.cell_slice<i32>>):
    // Nested: a two-global call site.
    // CHECK: emitrust.global_cells @g2
    emitrust.global_cells @g2 {
    ^bb0(%c2: !emitrust.ref<!emitrust.cell_slice<i32>>):
      // CHECK: emitrust.call_opaque "move2"(%{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.cell_slice<i32>>, !emitrust.ref<!emitrust.cell_slice<i32>>) -> ()
      emitrust.call_opaque "move2"(%c1, %c2) : (!emitrust.ref<!emitrust.cell_slice<i32>>, !emitrust.ref<!emitrust.cell_slice<i32>>) -> ()
      emitrust.yield
    }
    emitrust.yield
  }
  emitrust.return
}

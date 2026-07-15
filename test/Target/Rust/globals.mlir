// C99-14/C99-15: global emission: const globals render as plain static
// items read directly; mutable globals render as thread_local Cell items
// accessed through .with(), so the generated crate stays free of `unsafe`
// and `static mut`.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s --strict-whitespace

// CHECK:      thread_local! {
// CHECK-NEXT:     static counter: std::cell::Cell<i32> = std::cell::Cell::new(0);
// CHECK-NEXT: }
emitrust.global @counter <0 : i32> : i32

// CHECK-NEXT: static limit: i32 = 100;
emitrust.global const @limit <100 : i32> : i32

// A const global without an initializer starts at the type's default.
// CHECK-NEXT: static zero: i64 = 0;
emitrust.global const @zero : i64

// CHECK-NEXT: thread_local! {
// CHECK-NEXT:     static ratio: std::cell::Cell<f64> = std::cell::Cell::new(2.5);
// CHECK-NEXT: }
emitrust.global @ratio <2.5 : f64> : f64

// CHECK-NEXT: thread_local! {
// CHECK-NEXT:     static flag: std::cell::Cell<bool> = std::cell::Cell::new(true);
// CHECK-NEXT: }
emitrust.global @flag <true> : i1

// Zero-initialized array and struct globals use the type's default value.
// CHECK-NEXT: thread_local! {
// CHECK-NEXT:     static table: std::cell::Cell<[i32; 4]> = std::cell::Cell::new([0; 4]);
// CHECK-NEXT: }
emitrust.global @table : !emitrust.array<4xi32>

// CHECK-NEXT: static fixed: [f64; 2] = [0.0; 2];
emitrust.global const @fixed : !emitrust.array<2xf64>

// CHECK-NEXT: #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct Point {
// CHECK-NEXT:     x: i32,
// CHECK-NEXT:     y: i32,
// CHECK-NEXT: }
emitrust.struct_def @Point ["x", "y"] [i32, i32]

// CHECK-NEXT: thread_local! {
// CHECK-NEXT:     static origin: std::cell::Cell<Point> = std::cell::Cell::new(Point::default());
// CHECK-NEXT: }
emitrust.global @origin : !emitrust.struct<"Point">

// CHECK-NEXT: fn access() -> i32 {
emitrust.func @access() -> i32 {
  // A load from a mutable global goes through the Cell.
  // CHECK-NEXT:     let v0: i32 = counter.with(|c| c.get());
  %0 = emitrust.global_load @counter : i32
  // A store to a mutable global goes through the Cell.
  // CHECK-NEXT:     counter.with(|c| c.set(v0));
  emitrust.global_store %0, @counter : i32
  // A load from a const global is a direct read of the static item.
  // CHECK-NEXT:     let v1: i32 = limit;
  %1 = emitrust.global_load @limit : i32
  // Whole-aggregate loads and stores move the array value through the Cell.
  // CHECK-NEXT:     let v2: [i32; 4] = table.with(|c| c.get());
  %2 = emitrust.global_load @table : !emitrust.array<4xi32>
  // CHECK-NEXT:     table.with(|c| c.set(v2));
  emitrust.global_store %2, @table : !emitrust.array<4xi32>
  // CHECK-NEXT:     return v1;
  emitrust.return %1 : i32
}

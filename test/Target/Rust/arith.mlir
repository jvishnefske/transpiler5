// FR-5: Expression emission: constant, literal, call_opaque, add/sub/mul/div/rem, cmp, cast.
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

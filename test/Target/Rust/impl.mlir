// FR-30: Rust rendering of the owner-struct surface: emitrust.impl renders
// an inherent `impl` block whose functions take `&mut self` (the receiver
// argument is named `self`, so the deref place renders `(*self).data[...]`),
// and emitrust.method_call renders `place.method(args)` — as a `let`
// binding with a result, as a bare statement without one, and through a
// dereferenced receiver for sibling calls (`(*self).g(...)`).
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK: impl Owner_main_arr {
emitrust.impl "Owner_main_arr" {
  // CHECK: fn fill(&mut self, v0: i64, v1: i32) {
  emitrust.func @fill(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, %arg1: i64, %arg2: i32) {
    %s = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>
    %d = emitrust.member %s["data"] : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.array<8xi32>>
    %e = emitrust.subscript %d[%arg1] : (!emitrust.lvalue<!emitrust.array<8xi32>>, i64) -> !emitrust.lvalue<i32>
    // CHECK: (*self).data[v0 as usize] = v1;
    emitrust.assign %e = %arg2 : !emitrust.lvalue<i32>
    // A sibling method call through the dereferenced receiver, without a
    // result: a bare statement.
    // CHECK: (*self).g(v0);
    emitrust.method_call %s["g"] (%arg1) : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>, i64) -> ()
    emitrust.return
  }
  // CHECK: fn g(&mut self, _v0: i64) {
  emitrust.func @g(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, %arg1: i64) {
    emitrust.return
  }
  // CHECK: fn sum(&mut self, v0: i64) -> i32 {
  emitrust.func @sum(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, %arg1: i64) -> i32 {
    %s = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>
    %d = emitrust.member %s["data"] : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.array<8xi32>>
    %e = emitrust.subscript %d[%arg1] : (!emitrust.lvalue<!emitrust.array<8xi32>>, i64) -> !emitrust.lvalue<i32>
    // The load is the single-use producer of the returned value, so FR-61a
    // folds the binding: the place expression is the method's tail expression.
    // CHECK: (*self).data[v0 as usize]
    %v = emitrust.load %e : (!emitrust.lvalue<i32>) -> i32
    emitrust.return %v : i32
  }
// CHECK: }
}

// CHECK: fn c_main() -> i32 {
emitrust.func @c_main() -> i32 {
  // CHECK: let mut v0: Owner_main_arr = Owner_main_arr::default();
  %o = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>
  // The index constant is used twice, so it keeps its binding; the
  // single-use value constant inlines (suffixed) into the call (FR-61d).
  // CHECK: let v1: i64 = 2;
  %i = emitrust.constant <2 : i64> : i64
  %v = emitrust.constant <7 : i32> : i32
  // Method call without a result: a bare statement on the owner place.
  // CHECK: v0.fill(v1, 7i32);
  emitrust.method_call %o["fill"] (%i, %v) : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>, i64, i32) -> ()
  // Method call with a result feeding the final return: FR-61a folds the
  // binding and the call expression becomes the tail expression.
  // CHECK: v0.sum(v1)
  %r = emitrust.method_call %o["sum"] (%i) : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>, i64) -> i32
  emitrust.return %r : i32
}

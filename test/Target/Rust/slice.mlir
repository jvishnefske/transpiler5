// FR-28: Rust rendering of slice borrows and slice-place accesses:
// emitrust.slice_of renders `&place[idx as usize..]` (mutable with the mut
// marker, cast omitted for an index-typed index), and a subscript over a
// dereferenced slice place renders `(*ref)[idx as usize]`.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK-LABEL: fn slice_of_forms(
emitrust.func @slice_of_forms(%arg0: i64, %arg1: index) {
  %a = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  // CHECK: let _v3: &mut [i32] = &mut v2[v0 as usize..];
  %m = emitrust.slice_of mut %a[%arg0] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  // CHECK: let _v4: &[i32] = &v2[v0 as usize..];
  %r = emitrust.slice_of %a[%arg0] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.ref<!emitrust.slice<i32>>
  // The `as usize` cast is omitted for an index-typed index.
  // CHECK: let _v5: &mut [i32] = &mut v2[v1..];
  %i = emitrust.slice_of mut %a[%arg1] : (!emitrust.lvalue<!emitrust.array<4xi32>>, index) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  emitrust.return
}

// CHECK-LABEL: fn slice_place(v0: &mut [i32], v1: i64) -> i32 {
emitrust.func @slice_place(%arg0: !emitrust.mut_ref<!emitrust.slice<i32>>, %arg1: i64) -> i32 {
  %s = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.slice<i32>>) -> !emitrust.lvalue<!emitrust.slice<i32>>
  %e = emitrust.subscript %s[%arg1] : (!emitrust.lvalue<!emitrust.slice<i32>>, i64) -> !emitrust.lvalue<i32>
  // CHECK: let v2: i32 = v0[v1 as usize];
  %v = emitrust.load %e : (!emitrust.lvalue<i32>) -> i32
  // CHECK: v0[v1 as usize] = v2;
  emitrust.assign %e = %v : !emitrust.lvalue<i32>
  // Reslicing a dereferenced slice place composes.
  // CHECK: let _v3: &mut [i32] = &mut (*v0)[v1 as usize..];
  %t = emitrust.slice_of mut %s[%arg1] : (!emitrust.lvalue<!emitrust.slice<i32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  // CHECK: v2
  emitrust.return %v : i32
}

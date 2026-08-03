// FR-5: Correct Rust rendering of all supported builtin types plus opaque,
// ref, and mut_ref. The parameter signatures carry the pinned type
// spellings; the unused alias lets that once re-pinned them in statement
// position now drop entirely (FR-61d), un-reading their parameters.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK-LABEL: fn builtins(_v0: bool, _v1: i8, _v2: i16, _v3: i32, _v4: i64, _v5: u8, _v6: u16, _v7: u32, _v8: u64, _v9: usize, _v10: f32, _v11: f64) {
// CHECK-NEXT:  }
emitrust.func @builtins(%arg0: i1, %arg1: i8, %arg2: i16, %arg3: i32,
                        %arg4: i64, %arg5: ui8, %arg6: ui16, %arg7: ui32,
                        %arg8: ui64, %arg9: index, %arg10: f32, %arg11: f64) {
  %0 = emitrust.let %arg0 : i1
  %1 = emitrust.let %arg8 : ui64
  %2 = emitrust.let %arg9 : index
  emitrust.return
}

// CHECK-LABEL: fn references(_v0: String, _v1: &i32, _v2: &mut String) {
// CHECK-NEXT:  }
emitrust.func @references(%arg0: !emitrust.opaque<"String">,
                          %arg1: !emitrust.ref<i32>,
                          %arg2: !emitrust.mut_ref<!emitrust.opaque<"String">>) {
  %0 = emitrust.let %arg1 : !emitrust.ref<i32>
  %1 = emitrust.let %arg2 : !emitrust.mut_ref<!emitrust.opaque<"String">>
  emitrust.return
}

// CHECK-LABEL: fn nested_ref(_v0: &Vec<i32>) {
// CHECK-NEXT:  }
emitrust.func @nested_ref(%arg0: !emitrust.ref<!emitrust.opaque<"Vec<i32>">>) {
  %0 = emitrust.let %arg0 : !emitrust.ref<!emitrust.opaque<"Vec<i32>">>
  emitrust.return
}

// CHECK-LABEL: fn slice_params(_v0: &mut [i32], _v1: &[f64]) {
// CHECK-NEXT:  }
emitrust.func @slice_params(%arg0: !emitrust.mut_ref<!emitrust.slice<i32>>,
                            %arg1: !emitrust.ref<!emitrust.slice<f64>>) {
  %0 = emitrust.let %arg0 : !emitrust.mut_ref<!emitrust.slice<i32>>
  emitrust.return
}

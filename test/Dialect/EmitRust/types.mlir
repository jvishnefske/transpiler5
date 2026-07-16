// FR-2: Types parse and print: opaque, ref, mut_ref, including nesting.
// RUN: emitrust-opt %s | emitrust-opt | FileCheck %s

// CHECK-LABEL: emitrust.func @opaque_simple(
// CHECK-SAME: !emitrust.opaque<"String">
emitrust.func @opaque_simple(%arg0: !emitrust.opaque<"String">) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @opaque_generic(
// CHECK-SAME: !emitrust.opaque<"Vec<i32>">
emitrust.func @opaque_generic(%arg0: !emitrust.opaque<"Vec<i32>">) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @ref_builtin(
// CHECK-SAME: !emitrust.ref<i32>
emitrust.func @ref_builtin(%arg0: !emitrust.ref<i32>) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @mut_ref_opaque(
// CHECK-SAME: !emitrust.mut_ref<!emitrust.opaque<"String">>
emitrust.func @mut_ref_opaque(%arg0: !emitrust.mut_ref<!emitrust.opaque<"String">>) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @ref_of_opaque_generic(
// CHECK-SAME: !emitrust.ref<!emitrust.opaque<"Vec<i32>">>
emitrust.func @ref_of_opaque_generic(%arg0: !emitrust.ref<!emitrust.opaque<"Vec<i32>">>) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @mut_ref_of_ref(
// CHECK-SAME: !emitrust.mut_ref<!emitrust.ref<i32>>
emitrust.func @mut_ref_of_ref(%arg0: !emitrust.mut_ref<!emitrust.ref<i32>>) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @types_on_ops
emitrust.func @types_on_ops(%arg0: !emitrust.ref<i32>) {
  // CHECK: emitrust.let %{{.*}} : !emitrust.ref<i32>
  %0 = emitrust.let %arg0 : !emitrust.ref<i32>
  // CHECK: emitrust.literal "&mut buffer" : !emitrust.mut_ref<!emitrust.opaque<"Vec<u8>">>
  %1 = emitrust.literal "&mut buffer" : !emitrust.mut_ref<!emitrust.opaque<"Vec<u8>">>
  emitrust.return
}

// CHECK-LABEL: emitrust.func @array_param(
// CHECK-SAME: !emitrust.array<4xi32>
emitrust.func @array_param(%arg0: !emitrust.array<4xi32>) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @struct_param(
// CHECK-SAME: !emitrust.struct<"Point">
emitrust.func @struct_param(%arg0: !emitrust.struct<"Point">) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @array_of_struct_param(
// CHECK-SAME: !emitrust.array<2x!emitrust.struct<"Point">>
emitrust.func @array_of_struct_param(%arg0: !emitrust.array<2x!emitrust.struct<"Point">>) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @lvalue_types
emitrust.func @lvalue_types() {
  // CHECK: emitrust.variable : !emitrust.lvalue<i32>
  %0 = emitrust.variable : !emitrust.lvalue<i32>
  // CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  %1 = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  // CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Point">>
  %2 = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Point">>
  // CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<2x!emitrust.struct<"Point">>>
  %3 = emitrust.variable : !emitrust.lvalue<!emitrust.array<2x!emitrust.struct<"Point">>>
  emitrust.return
}

// CHECK-LABEL: emitrust.func @enum_param(
// CHECK-SAME: !emitrust.enum<"Color">
// CHECK-SAME: -> !emitrust.enum<"Color">
emitrust.func @enum_param(%arg0: !emitrust.enum<"Color">) -> !emitrust.enum<"Color"> {
  emitrust.return %arg0 : !emitrust.enum<"Color">
}

// CHECK: emitrust.struct_def @Pixel ["pos", "color"] [i32, !emitrust.enum<"Color">]
emitrust.struct_def @Pixel ["pos", "color"] [i32, !emitrust.enum<"Color">]

// CHECK-LABEL: emitrust.func @enum_lvalue
emitrust.func @enum_lvalue() {
  // CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.enum<"Color">>
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.enum<"Color">>
  emitrust.return
}

// CHECK-LABEL: emitrust.func @slice_param(
// CHECK-SAME: !emitrust.mut_ref<!emitrust.slice<i32>>
emitrust.func @slice_param(%arg0: !emitrust.mut_ref<!emitrust.slice<i32>>) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @slice_of_struct_param(
// CHECK-SAME: !emitrust.ref<!emitrust.slice<!emitrust.struct<"Point">>>
emitrust.func @slice_of_struct_param(%arg0: !emitrust.ref<!emitrust.slice<!emitrust.struct<"Point">>>) {
  emitrust.return
}

// A slice may be the value type of an lvalue (behind a deref).
// CHECK-LABEL: emitrust.func @slice_lvalue(
emitrust.func @slice_lvalue(%arg0: !emitrust.mut_ref<!emitrust.slice<f64>>) {
  // CHECK: emitrust.deref %{{.*}} : (!emitrust.mut_ref<!emitrust.slice<f64>>) -> !emitrust.lvalue<!emitrust.slice<f64>>
  %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.slice<f64>>) -> !emitrust.lvalue<!emitrust.slice<f64>>
  emitrust.return
}

// CHECK-LABEL: emitrust.func @fn_ptr_full(
// CHECK-SAME: !emitrust.fn_ptr<(i32, i32) -> i32>
emitrust.func @fn_ptr_full(%arg0: !emitrust.fn_ptr<(i32, i32) -> i32>) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @fn_ptr_zero_arg(
// CHECK-SAME: !emitrust.fn_ptr<() -> i32>
emitrust.func @fn_ptr_zero_arg(%arg0: !emitrust.fn_ptr<() -> i32>) {
  emitrust.return
}

// The void-result spelling omits the `->` clause entirely.
// CHECK-LABEL: emitrust.func @fn_ptr_void_result(
// CHECK-SAME: !emitrust.fn_ptr<(i32)>
emitrust.func @fn_ptr_void_result(%arg0: !emitrust.fn_ptr<(i32)>) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @fn_ptr_zero_arg_void(
// CHECK-SAME: !emitrust.fn_ptr<()>
emitrust.func @fn_ptr_zero_arg_void(%arg0: !emitrust.fn_ptr<()>) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @fn_ptr_mixed_components(
// CHECK-SAME: !emitrust.fn_ptr<(i1, ui64, index, f64, !emitrust.struct<"Point">, !emitrust.enum<"Color">) -> ui32>
emitrust.func @fn_ptr_mixed_components(
    %arg0: !emitrust.fn_ptr<(i1, ui64, index, f64, !emitrust.struct<"Point">,
                             !emitrust.enum<"Color">) -> ui32>) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @fn_ptr_nested(
// CHECK-SAME: !emitrust.fn_ptr<(!emitrust.fn_ptr<(i32) -> i32>) -> !emitrust.fn_ptr<() -> i32>>
emitrust.func @fn_ptr_nested(
    %arg0: !emitrust.fn_ptr<(!emitrust.fn_ptr<(i32) -> i32>)
                            -> !emitrust.fn_ptr<() -> i32>>) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @fn_ptr_lvalue
emitrust.func @fn_ptr_lvalue() {
  // CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  emitrust.return
}

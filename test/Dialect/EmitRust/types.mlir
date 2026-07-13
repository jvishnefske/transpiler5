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

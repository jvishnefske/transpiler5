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

// An array of enums (C `enum Color c[3]`) must round-trip: the importer
// creates this type for enum-typed array locals/globals, so a printer that
// emits it but a parser that rejected it broke every serialize-reload path
// (FR-57 discovered this as its first spike finding).
// CHECK-LABEL: emitrust.func @array_of_enum_param(
// CHECK-SAME: !emitrust.array<3x!emitrust.enum<"Color">>
emitrust.func @array_of_enum_param(%arg0: !emitrust.array<3x!emitrust.enum<"Color">>) {
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

// FR-76 retro-pin: a region-typed slice borrow is a valid component and
// has had NO round-trip coverage anywhere in test/Dialect (the only
// reference-component coverage was the negative at invalid.mlir).
// CHECK-LABEL: emitrust.func @fn_ptr_slice_components(
// CHECK-SAME: !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.slice<ui8>>, !emitrust.ref<!emitrust.slice<ui8>>, ui32)>
emitrust.func @fn_ptr_slice_components(
    %arg0: !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.slice<ui8>>,
                             !emitrust.ref<!emitrust.slice<ui8>>, ui32)>) {
  emitrust.return
}

// FR-102: a `ref`/`mut_ref` borrow of a named STRUCT is a valid component.
// The Rust spelling `fn(&mut Node, i32) -> i32` is higher-ranked with
// elided lifetimes, so no annotation is carried in the type.
// CHECK-LABEL: emitrust.func @fn_ptr_struct_ref_components(
// CHECK-SAME: !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"Node">>, !emitrust.ref<!emitrust.struct<"Point">>, i32) -> i32>
emitrust.func @fn_ptr_struct_ref_components(
    %arg0: !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"Node">>,
                             !emitrust.ref<!emitrust.struct<"Point">>,
                             i32) -> i32>) {
  emitrust.return
}

// FR-102: the same component in a struct_def FIELD position — the shape
// the C importer actually emits for `struct node { int (*visit)(struct
// node *, int); }`, including the SELF-REFERENCE (the field names the
// struct_def being defined; struct types are by-name, so this is legal).
// CHECK: emitrust.struct_def @Node ["val", "visit"] [i32, !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"Node">>, i32) -> i32>]
emitrust.struct_def @Node ["val", "visit"]
    [i32, !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"Node">>,
                            i32) -> i32>]

// FR-102: mixing a slice borrow and a struct borrow in one signature is
// the shape the corpus sweep produced (`Option<fn(&mut Fsm, &mut [u8])>`).
// CHECK-LABEL: emitrust.func @fn_ptr_mixed_borrow_components(
// CHECK-SAME: !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"Fsm">>, !emitrust.mut_ref<!emitrust.slice<ui8>>) -> ui32>
emitrust.func @fn_ptr_mixed_borrow_components(
    %arg0: !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"Fsm">>,
                             !emitrust.mut_ref<!emitrust.slice<ui8>>)
                            -> ui32>) {
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

// CHECK-LABEL: emitrust.func @nested_array_param(
// CHECK-SAME: !emitrust.array<2x!emitrust.array<4xi8>>
emitrust.func @nested_array_param(%arg0: !emitrust.array<2x!emitrust.array<4xi8>>) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @nested_array_three_levels(
// CHECK-SAME: !emitrust.array<2x!emitrust.array<3x!emitrust.array<5xi32>>>
emitrust.func @nested_array_three_levels(
    %arg0: !emitrust.array<2x!emitrust.array<3x!emitrust.array<5xi32>>>) {
  emitrust.return
}

// CHECK-LABEL: emitrust.func @nested_array_places
emitrust.func @nested_array_places(%arg0: index) {
  // CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<2x!emitrust.array<4xi8>>>
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.array<2x!emitrust.array<4xi8>>>
  // Each subscript peels one array level.
  // CHECK: emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<2x!emitrust.array<4xi8>>>, index) -> !emitrust.lvalue<!emitrust.array<4xi8>>
  %1 = emitrust.subscript %0[%arg0] : (!emitrust.lvalue<!emitrust.array<2x!emitrust.array<4xi8>>>, index) -> !emitrust.lvalue<!emitrust.array<4xi8>>
  // CHECK: emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi8>>, index) -> !emitrust.lvalue<i8>
  %2 = emitrust.subscript %1[%arg0] : (!emitrust.lvalue<!emitrust.array<4xi8>>, index) -> !emitrust.lvalue<i8>
  emitrust.return
}

// CHECK: emitrust.global @grid <{{\[}}[0 : i8, 1 : i8], [2 : i8, 3 : i8], [4 : i8, 5 : i8]]> : !emitrust.array<3x!emitrust.array<2xi8>>
emitrust.global @grid <[[0 : i8, 1 : i8], [2 : i8, 3 : i8], [4 : i8, 5 : i8]]> : !emitrust.array<3x!emitrust.array<2xi8>>

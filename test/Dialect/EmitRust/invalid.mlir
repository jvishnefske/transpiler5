// FR-3: Op verifiers reject malformed IR with precise diagnostics.
// RUN: emitrust-opt %s --split-input-file --verify-diagnostics

emitrust.func @assign_to_block_arg(%arg0: i32, %arg1: i32) {
  // expected-error @+1 {{destination is not produced by an emitrust.let}}
  emitrust.assign %arg0 = %arg1 : i32
  emitrust.return
}

// -----

emitrust.func @assign_to_immutable_let(%arg0: i32) {
  %0 = emitrust.let %arg0 : i32
  // expected-error @+1 {{destination let binding is not mutable}}
  emitrust.assign %0 = %arg0 : i32
  emitrust.return
}

// -----

emitrust.func @assign_to_constant(%arg0: i32) {
  %0 = emitrust.constant <1 : i32> : i32
  // expected-error @+1 {{destination is not produced by an emitrust.let}}
  emitrust.assign %0 = %arg0 : i32
  emitrust.return
}

// -----

emitrust.func @empty_literal() {
  // expected-error @+1 {{empty}}
  %0 = emitrust.literal "" : i32
  emitrust.return
}

// -----

emitrust.func @empty_opaque_type() {
  // expected-error @+1 {{empty}}
  %0 = emitrust.literal "x" : !emitrust.opaque<"">
  emitrust.return
}

// -----

emitrust.func @break_outside_loop() {
  // expected-error @+1 {{must appear inside an emitrust.loop, emitrust.for, or emitrust.while}}
  emitrust.break
  emitrust.return
}

// -----

emitrust.func @continue_outside_loop(%arg0: i1) {
  emitrust.if %arg0 {
    // expected-error @+1 {{must appear inside an emitrust.loop, emitrust.for, or emitrust.while}}
    emitrust.continue
  }
  emitrust.return
}

// -----

// expected-error @+1 {{argument #0 must not be of lvalue type}}
emitrust.func @lvalue_param(%arg0: !emitrust.lvalue<i32>) {
  emitrust.return
}

// -----

// expected-error @+1 {{result must not be of lvalue type}}
emitrust.func @lvalue_result() -> !emitrust.lvalue<i32> {
  %0 = emitrust.variable : !emitrust.lvalue<i32>
  emitrust.return %0 : !emitrust.lvalue<i32>
}

// -----

emitrust.func @member_on_non_struct() {
  %0 = emitrust.variable : !emitrust.lvalue<i32>
  // expected-error @+1 {{operand must be an lvalue of !emitrust.struct type}}
  %1 = emitrust.member %0["x"] : (!emitrust.lvalue<i32>) -> !emitrust.lvalue<i32>
  emitrust.return
}

// -----

emitrust.func @subscript_on_non_array(%arg0: index) {
  %0 = emitrust.variable : !emitrust.lvalue<i32>
  // expected-error @+1 {{operand must be an lvalue of !emitrust.array, !emitrust.slice, or !emitrust.opaque type}}
  %1 = emitrust.subscript %0[%arg0] : (!emitrust.lvalue<i32>, index) -> !emitrust.lvalue<i32>
  emitrust.return
}

// -----

emitrust.func @assign_lvalue_type_mismatch(%arg0: i64) {
  %0 = emitrust.variable : !emitrust.lvalue<i32>
  // expected-error @+1 {{value type 'i64' does not match the destination value type 'i32'}}
  "emitrust.assign"(%0, %arg0) : (!emitrust.lvalue<i32>, i64) -> ()
  emitrust.return
}

// -----

emitrust.func @addr_of_mut_ref_mismatch() {
  %0 = emitrust.variable : !emitrust.lvalue<i32>
  // expected-error @+1 {{mut marker requires a !emitrust.mut_ref result type}}
  %1 = emitrust.addr_of mut %0 : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
  emitrust.return
}

// -----

emitrust.func @addr_of_missing_mut() {
  %0 = emitrust.variable : !emitrust.lvalue<i32>
  // expected-error @+1 {{result is a !emitrust.mut_ref but the mut marker is absent}}
  %1 = emitrust.addr_of %0 : (!emitrust.lvalue<i32>) -> !emitrust.mut_ref<i32>
  emitrust.return
}

// -----

// expected-error @+1 {{has 1 field names but 2 field types}}
emitrust.struct_def @Mismatched ["x"] [i32, i32]

// -----

emitrust.func @variable_init_type_mismatch() {
  // expected-error @+1 {{init type 'i64' does not match the variable value type 'i32'}}
  %0 = emitrust.variable <42 : i64> : !emitrust.lvalue<i32>
  emitrust.return
}

// -----

emitrust.func @call_args_out_of_range(%arg0: i32) {
  // expected-error @+1 {{args index 1 is out of range}}
  emitrust.call_opaque "f"(%arg0) {args = [1 : index]} : (i32) -> ()
  emitrust.return
}

// -----

emitrust.func @switch_region_mismatch(%arg0: index) {
  // expected-error @+1 {{'emitrust.switch' op has 1 case regions but 2 case values}}
  "emitrust.switch"(%arg0) <{cases = array<i64: 0, 1>}> ({
    "emitrust.yield"() : () -> ()
  }, {
    "emitrust.yield"() : () -> ()
  }) : (index) -> ()
  emitrust.return
}

// -----

emitrust.func @switch_duplicate_case(%arg0: i32) {
  // expected-error @+1 {{has duplicate case value 3}}
  emitrust.switch %arg0 : i32
  case 3 {
  }
  case 3 {
  }
  default {
  }
  emitrust.return
}

// -----

emitrust.func @enum_ordered_cmp(%arg0: !emitrust.enum<"Color">, %arg1: !emitrust.enum<"Color">) {
  // expected-error @+1 {{enum operands only support the eq and ne predicates}}
  %0 = emitrust.cmp lt, %arg0, %arg1 : (!emitrust.enum<"Color">, !emitrust.enum<"Color">) -> i1
  emitrust.return
}

// -----

emitrust.func @cast_to_enum_from_float(%arg0: f64) {
  // expected-error @+1 {{a cast to an enum type requires a non-i1 integer source}}
  %0 = emitrust.cast %arg0 : f64 to !emitrust.enum<"Color">
  emitrust.return
}

// -----

emitrust.func @cast_to_enum_from_bool(%arg0: i1) {
  // expected-error @+1 {{a cast to an enum type requires a non-i1 integer source}}
  %0 = emitrust.cast %arg0 : i1 to !emitrust.enum<"Color">
  emitrust.return
}

// -----

emitrust.func @bitcast_int_to_int(%arg0: i32) {
  // expected-error @+1 {{requires one float operand or result and one integer of the same width}}
  %0 = emitrust.bitcast %arg0 : i32 to ui32
  emitrust.return
}

// -----

emitrust.func @bitcast_float_to_float(%arg0: f32) {
  // expected-error @+1 {{requires one float operand or result and one integer of the same width}}
  %0 = emitrust.bitcast %arg0 : f32 to f64
  emitrust.return
}

// -----

emitrust.func @bitcast_width_mismatch(%arg0: f32) {
  // expected-error @+1 {{float and integer sides must have the same width}}
  %0 = emitrust.bitcast %arg0 : f32 to ui64
  emitrust.return
}

// -----

// expected-error @+1 {{variant value -1 is negative but the enum has an unsigned underlying type}}
emitrust.enum_def @Neg ["A"] [-1] {unsigned_underlying}

// -----

emitrust.func @enum_raw_not_enum(%arg0: i32) {
  %0 = emitrust.variable : !emitrust.lvalue<i32>
  // expected-error @+1 {{operand must be an lvalue of !emitrust.enum type}}
  %1 = emitrust.enum_raw %0 : (!emitrust.lvalue<i32>) -> !emitrust.lvalue<i32>
  emitrust.return
}

// -----

emitrust.func @enum_raw_bad_result() {
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.enum<"Color">>
  // expected-error @+1 {{result must be an lvalue of a 32-bit integer type}}
  %1 = emitrust.enum_raw %0 : (!emitrust.lvalue<!emitrust.enum<"Color">>) -> !emitrust.lvalue<i64>
  emitrust.return
}

// -----

emitrust.func @select_type_mismatch(%arg0: i1, %arg1: i32, %arg2: i64) {
  // expected-error @+1 {{failed to verify that all of {trueValue, falseValue, result} have same type}}
  %0 = "emitrust.select"(%arg0, %arg1, %arg2) : (i1, i32, i64) -> i32
  emitrust.return
}

// -----

emitrust.func @select_on_lvalue(%arg0: i1) {
  %0 = emitrust.variable : !emitrust.lvalue<i32>
  %1 = emitrust.variable : !emitrust.lvalue<i32>
  // expected-error @+1 {{op operand #1 must be}}
  %2 = "emitrust.select"(%arg0, %0, %1) : (i1, !emitrust.lvalue<i32>, !emitrust.lvalue<i32>) -> !emitrust.lvalue<i32>
  emitrust.return
}

// -----

// expected-error @+1 {{duplicate variant name "Red"}}
emitrust.enum_def @DupName ["Red", "Red"] [0, 1]

// -----

// expected-error @+1 {{must have at least one variant}}
emitrust.enum_def @Empty [] []

// -----

// The bitwise and shift operations only accept arithmetic types; an lvalue
// operand is rejected by the ODS type constraint.
emitrust.func @and_on_lvalue() {
  %0 = emitrust.variable : !emitrust.lvalue<i32>
  %1 = emitrust.variable : !emitrust.lvalue<i32>
  // expected-error @+1 {{op operand #0 must be}}
  %2 = "emitrust.and"(%0, %1) : (!emitrust.lvalue<i32>, !emitrust.lvalue<i32>) -> !emitrust.lvalue<i32>
  emitrust.return
}

// -----

emitrust.func @shr_on_lvalue() {
  %0 = emitrust.variable : !emitrust.lvalue<i32>
  %1 = emitrust.variable : !emitrust.lvalue<i32>
  // expected-error @+1 {{op operand #0 must be}}
  %2 = "emitrust.shr"(%0, %1) : (!emitrust.lvalue<i32>, !emitrust.lvalue<i32>) -> !emitrust.lvalue<i32>
  emitrust.return
}

// -----

// expected-error @+1 {{invalid global value type '!emitrust.ref<i32>'}}
emitrust.global @bad_type : !emitrust.ref<i32>

// -----

// expected-error @+1 {{const marker requires a scalar or array value type, but got '!emitrust.struct<"Point">'}}
emitrust.global const @const_struct : !emitrust.struct<"Point">

// -----

// expected-error @+1 {{init type 'i64' does not match the global value type 'i32'}}
emitrust.global @init_mismatch <42 : i64> : i32

// -----

// expected-error @+1 {{init is only supported for scalar value types}}
emitrust.global @aggregate_init <42 : i32> : !emitrust.array<4xi32>

// -----

// expected-error @+1 {{aggregate init has 2 elements, but the array type '!emitrust.array<4xi32>' has 4}}
emitrust.global @short_list <[1 : i32, 2 : i32]> : !emitrust.array<4xi32>

// -----

// expected-error @+1 {{aggregate init element type 'i64' does not match the expected type 'i32'}}
emitrust.global @element_mismatch <[1 : i32, 2 : i64]> : !emitrust.array<2xi32>

// -----

// expected-error @+1 {{list init is only supported for array and struct value types, but got 'i32'}}
emitrust.global @list_on_scalar <[1 : i32]> : i32

// -----

// expected-error @+1 {{aggregate init for struct type '!emitrust.struct<"Ghost">' requires a visible emitrust.struct_def}}
emitrust.global @no_struct_def <[1 : i32]> : !emitrust.struct<"Ghost">

// -----

// FR-84: the module-first lookup still REJECTS a genuinely missing def —
// an impl-nested aggregate init only resolves defs the module actually
// holds, with the same wording as the module-level case above.
emitrust.impl "Owner" {
  emitrust.func @stage(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner">>) {
    // expected-error @+1 {{aggregate init for struct type '!emitrust.struct<"Ghost">' requires a visible emitrust.struct_def}}
    %0 = emitrust.variable const <[1 : i32]> : !emitrust.lvalue<!emitrust.struct<"Ghost">>
    emitrust.return
  }
}

// -----

emitrust.struct_def @Pair ["a", "b"] [i32, i32]
// expected-error @+1 {{aggregate init has 1 elements, but struct 'Pair' has 2 fields}}
emitrust.global @field_count <[1 : i32]> : !emitrust.struct<"Pair">

// -----

emitrust.func @load_unknown_symbol() {
  // expected-error @+1 {{'missing' does not reference a valid emitrust.global}}
  %0 = emitrust.global_load @missing : i32
  emitrust.return
}

// -----

emitrust.struct_def @NotAGlobal ["x"] [i32]
emitrust.func @load_wrong_symbol_kind() {
  // expected-error @+1 {{'NotAGlobal' does not reference a valid emitrust.global}}
  %0 = emitrust.global_load @NotAGlobal : i32
  emitrust.return
}

// -----

emitrust.global @g_load_mismatch <0 : i32> : i32
emitrust.func @load_type_mismatch() {
  // expected-error @+1 {{result type 'i64' does not match the value type 'i32' of the global @g_load_mismatch}}
  %0 = emitrust.global_load @g_load_mismatch : i64
  emitrust.return
}

// -----

emitrust.global @g_store_mismatch <0 : i32> : i32
emitrust.func @store_type_mismatch(%arg0: i64) {
  // expected-error @+1 {{value type 'i64' does not match the value type 'i32' of the global @g_store_mismatch}}
  emitrust.global_store %arg0, @g_store_mismatch : i64
  emitrust.return
}

// -----

emitrust.global const @g_immutable <7 : i32> : i32
emitrust.func @store_to_immutable(%arg0: i32) {
  // expected-error @+1 {{cannot store to the immutable global @g_immutable}}
  emitrust.global_store %arg0, @g_immutable : i32
  emitrust.return
}

// -----

// FR-80: global_addr must name a real global.
emitrust.func @addr_unknown_symbol() {
  // expected-error @+1 {{'missing' does not reference a valid emitrust.global}}
  %0 = emitrust.global_addr @missing : !emitrust.ref<i32>
  emitrust.return
}

// -----

// FR-80: the reference pointee must be the global's value type.
emitrust.global const @g_addr_mismatch <0 : i32> : i32
emitrust.func @addr_type_mismatch() {
  // expected-error @+1 {{result pointee type 'i64' does not match the value type 'i32' of the global @g_addr_mismatch}}
  %0 = emitrust.global_addr @g_addr_mismatch : !emitrust.ref<i64>
  emitrust.return
}

// -----

// FR-80: only a const global has a stable SHARED address to lend — a
// mutable global's storage is a thread-local Cell with no lendable `&T`.
emitrust.global @g_addr_mut <0 : i32> : i32
emitrust.func @addr_of_mutable() {
  // expected-error @+1 {{cannot take the address of the mutable global @g_addr_mut}}
  %0 = emitrust.global_addr @g_addr_mut : !emitrust.ref<i32>
  emitrust.return
}

// -----

emitrust.func @slice_variable() {
  // expected-error @+1 {{variable value type must be sized, but got the slice type '!emitrust.slice<i32>'}}
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.slice<i32>>
  emitrust.return
}

// -----

emitrust.func @slice_bad_element(%arg0: !emitrust.mut_ref<!emitrust.slice<i32>>) {
  // expected-error @+2 {{invalid slice element type '!emitrust.slice<i32>'}}
  // expected-error @+1 {{failed to parse EmitRust_LValueType parameter 'value_type'}}
  %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.slice<i32>>) -> !emitrust.lvalue<!emitrust.slice<!emitrust.slice<i32>>>
  emitrust.return
}

// -----

emitrust.func @slice_of_non_array(%arg0: i64) {
  %0 = emitrust.variable : !emitrust.lvalue<i32>
  // FR-94: the admitted base set grew the opaque arm (the owned `Vec<u8>`
  // FAM tail slices like a member array); a scalar base stays rejected.
  // expected-error @+1 {{base must be an lvalue of !emitrust.array, !emitrust.slice, or !emitrust.opaque type}}
  %1 = emitrust.slice_of mut %0[%arg0] : (!emitrust.lvalue<i32>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  emitrust.return
}

// -----

emitrust.func @slice_of_missing_mut(%arg0: i64) {
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  // expected-error @+1 {{result is a !emitrust.mut_ref but the mut marker is absent}}
  %1 = emitrust.slice_of %0[%arg0] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  emitrust.return
}

// -----

emitrust.func @slice_of_stray_mut(%arg0: i64) {
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  // expected-error @+1 {{mut marker requires a !emitrust.mut_ref result type}}
  %1 = emitrust.slice_of mut %0[%arg0] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.ref<!emitrust.slice<i32>>
  emitrust.return
}

// -----

emitrust.func @slice_of_non_slice_result(%arg0: i64) {
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  // expected-error @+1 {{result pointee must be an !emitrust.slice, but got 'i32'}}
  %1 = emitrust.slice_of mut %0[%arg0] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.mut_ref<i32>
  emitrust.return
}

// -----

emitrust.func @slice_of_element_mismatch(%arg0: i64) {
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  // expected-error @+1 {{result slice element type 'i64' does not match the base element type 'i32'}}
  %1 = emitrust.slice_of mut %0[%arg0] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i64>>
  emitrust.return
}

// -----

emitrust.func @subscript_slice_element_mismatch(%arg0: !emitrust.mut_ref<!emitrust.slice<i32>>, %arg1: i64) {
  %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.slice<i32>>) -> !emitrust.lvalue<!emitrust.slice<i32>>
  // expected-error @+1 {{result value type 'i64' does not match the element type 'i32'}}
  %1 = emitrust.subscript %0[%arg1] : (!emitrust.lvalue<!emitrust.slice<i32>>, i64) -> !emitrust.lvalue<i64>
  emitrust.return
}

// -----

// A BARE SCALAR borrow is not a component: a fn-ptr signature is a
// contract with unknown implementors, and the region contract is what a C
// pointer parameter means. FR-102 widened the borrow set to `struct`
// pointees only; this stays invalid.
// expected-error @+1 {{invalid fn_ptr parameter type '!emitrust.ref<i32>'}}
emitrust.func @fn_ptr_bad_component(%arg0: !emitrust.fn_ptr<(!emitrust.ref<i32>) -> i32>) {
  emitrust.return
}

// -----

// FR-102 C3: reference-to-ENUM is deliberately OUT of the component set.
// A C `enum T *` component never reaches the record arm (clang calls an
// enum arithmetic, so it takes the FR-76 slice branch), which makes this
// unreachable from the importer and therefore unpinnable dialect surface.
// expected-error @+1 {{invalid fn_ptr parameter type '!emitrust.ref<!emitrust.enum<"Color">>'}}
emitrust.func @fn_ptr_enum_ref_component(%arg0: !emitrust.fn_ptr<(!emitrust.ref<!emitrust.enum<"Color">>) -> i32>) {
  emitrust.return
}

// -----

// A `data_enum` borrow stays invalid too: owner promotion turns a
// self-referential record's struct-pointer parameter into an (owner
// receiver, index) PAIR, which no fn_ptr component can carry.
// expected-error @+1 {{invalid fn_ptr parameter type '!emitrust.mut_ref<!emitrust.data_enum<"Msg">>'}}
emitrust.func @fn_ptr_data_enum_ref_component(%arg0: !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.data_enum<"Msg">>)>) {
  emitrust.return
}

// -----

// A borrow of a borrow is not a component (the pointer-to-pointer
// frontier's dialect half).
// expected-error @+1 {{invalid fn_ptr parameter type '!emitrust.mut_ref<!emitrust.mut_ref<!emitrust.struct<"S">>>'}}
emitrust.func @fn_ptr_ref_of_ref_component(%arg0: !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.mut_ref<!emitrust.struct<"S">>>)>) {
  emitrust.return
}

// -----

// expected-error @+1 {{invalid fn_ptr result type '!emitrust.array<4xi32>'}}
emitrust.func @fn_ptr_bad_result(%arg0: !emitrust.fn_ptr<() -> !emitrust.array<4xi32>>) {
  emitrust.return
}

// -----

emitrust.func @call_indirect_arg_count(%arg0: !emitrust.fn_ptr<(i32, i32) -> i32>, %arg1: i32) {
  // expected-error @+1 {{has 1 arguments, but the callee expects 2}}
  %0 = emitrust.call_indirect %arg0(%arg1) : (!emitrust.fn_ptr<(i32, i32) -> i32>, i32) -> i32
  emitrust.return
}

// -----

emitrust.func @call_indirect_arg_type(%arg0: !emitrust.fn_ptr<(i32) -> i32>, %arg1: i64) {
  // expected-error @+1 {{argument #0 type 'i64' does not match the callee parameter type 'i32'}}
  %0 = emitrust.call_indirect %arg0(%arg1) : (!emitrust.fn_ptr<(i32) -> i32>, i64) -> i32
  emitrust.return
}

// -----

emitrust.func @call_indirect_result_type(%arg0: !emitrust.fn_ptr<() -> i32>) {
  // expected-error @+1 {{result type 'i64' does not match the callee result type 'i32'}}
  %0 = emitrust.call_indirect %arg0() : (!emitrust.fn_ptr<() -> i32>) -> i64
  emitrust.return
}

// -----

emitrust.func @call_indirect_two_results(%arg0: !emitrust.fn_ptr<() -> i32>) {
  // expected-error @+1 {{has 2 results, but the callee produces 1}}
  %0:2 = emitrust.call_indirect %arg0() : (!emitrust.fn_ptr<() -> i32>) -> (i32, i32)
  emitrust.return
}

// -----

emitrust.func @fn_ptr_ordered_cmp(%arg0: !emitrust.fn_ptr<() -> i32>, %arg1: !emitrust.fn_ptr<() -> i32>) {
  // expected-error @+1 {{fn_ptr operands only support the eq and ne predicates}}
  %0 = emitrust.cmp lt, %arg0, %arg1 : (!emitrust.fn_ptr<() -> i32>, !emitrust.fn_ptr<() -> i32>) -> i1
  emitrust.return
}

// -----

emitrust.func @fn_ptr_cast(%arg0: !emitrust.fn_ptr<() -> i32>) {
  // expected-error @+1 {{cannot cast a fn_ptr type}}
  %0 = emitrust.cast %arg0 : !emitrust.fn_ptr<() -> i32> to i64
  emitrust.return
}

// -----

// expected-error @+1 {{const marker requires a scalar or array value type}}
emitrust.global const @const_fn_ptr : !emitrust.fn_ptr<() -> i32>

// -----

emitrust.func @method_call_non_struct(%arg0: i64) {
  %v = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  // expected-error @+1 {{receiver must be an lvalue of !emitrust.struct or !emitrust.opaque type}}
  emitrust.method_call %v["get"] (%arg0) : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> ()
  emitrust.return
}

// -----

emitrust.func @method_call_two_results() {
  %v = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Owner">>
  // expected-error @+1 {{requires zero or exactly one result, but has 2}}
  %0:2 = emitrust.method_call %v["get"] () : (!emitrust.lvalue<!emitrust.struct<"Owner">>) -> (i32, i32)
  emitrust.return
}

// -----

emitrust.impl "Owner" {
  // expected-error @+1 {{receiver must be a !emitrust.mut_ref of !emitrust.struct<"Owner">, but got '!emitrust.mut_ref<!emitrust.struct<"Other">>'}}
  emitrust.func @wrong_receiver(%arg0: !emitrust.mut_ref<!emitrust.struct<"Other">>) {
    emitrust.return
  }
}

// -----

emitrust.impl "Owner" {
  // expected-error @+1 {{method must take the receiver as its first argument}}
  emitrust.func @no_receiver() {
    emitrust.return
  }
}

// -----

emitrust.func @impl_not_at_module_level() {
  // expected-error @+1 {{'emitrust.impl' op expects parent op 'builtin.module'}}
  emitrust.impl "Owner" {
    emitrust.func @m(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner">>) {
      emitrust.return
    }
  }
  emitrust.return
}

// -----

// A slice never views runs of rows: nested arrays are valid array elements
// but invalid slice elements.
emitrust.func @slice_of_array_element(
    // expected-error @+2 {{invalid slice element type '!emitrust.array<4xi8>'}}
    // expected-error @+1 {{failed to parse EmitRust_MutRefType parameter 'pointee'}}
    %arg0: !emitrust.mut_ref<!emitrust.slice<!emitrust.array<4xi8>>>) {
  emitrust.return
}

// -----

// A nested aggregate initializer must match every level's extent.
// expected-error @+1 {{aggregate init has 3 elements, but the array type '!emitrust.array<4xi8>' has 4}}
emitrust.global @ragged <[[0 : i8, 1 : i8, 2 : i8], [3 : i8, 4 : i8, 5 : i8]]> : !emitrust.array<2x!emitrust.array<4xi8>>

// -----

// FR-52: the two requirement arrays must have the same length.
// expected-error @+1 {{has 2 method names but 1 method types}}
emitrust.trait_def @Externals ["a", "b"] [(i32) -> i32]

// -----

// FR-52: a requirement trait with nothing in it is never created by the
// lowering and is not a legal hand-written shape either.
// expected-error @+1 {{must declare at least one method}}
emitrust.trait_def @Externals [] []

// -----

// FR-52: two requirements cannot share a Rust name.
// expected-error @+1 {{duplicate method name "f"}}
emitrust.trait_def @Externals ["f", "f"] [(i32) -> i32, (i32) -> i32]

// -----

// FR-52: a requirement's type must be a function type.
// expected-error @+1 {{method "f" must have a function type}}
emitrust.trait_def @Externals ["f"] [i32]

// -----

// FR-52: the emitter renders at most one result, so neither may a
// requirement declare more.
// expected-error @+1 {{method "f" cannot have more than one result}}
emitrust.trait_def @Externals ["f"] [(i32) -> (i32, i32)]

// -----

// FR-61e: a carried variable name must be a valid Rust identifier.
emitrust.func @bad_variable_name() {
  // expected-error @+1 {{variable name must be a non-empty Rust identifier}}
  %0 = emitrust.variable named "1bad" : !emitrust.lvalue<i32>
  emitrust.return
}

// -----

// FR-61e: an empty carried name is rejected, never silently anonymous.
emitrust.func @empty_variable_name() {
  // expected-error @+1 {{variable name must be a non-empty Rust identifier}}
  %0 = emitrust.variable named "" : !emitrust.lvalue<i32>
  emitrust.return
}

// -----

// FR-61c: the while condition region must end in emitrust.condition.
emitrust.func @while_bad_terminator(%arg0: i32, %arg1: i32) {
  // expected-error @+1 {{condition region must be terminated by emitrust.condition}}
  emitrust.while {
    emitrust.yield
  } do {
    emitrust.yield
  }
  emitrust.return
}

// -----

// W2.17: `trait_name` on an `emitrust.impl` names the ONE trait impl the
// dialect models. Anything else would render `impl <whatever> for T` with
// a body the emitter has no contract for, so it is refused structurally.
// expected-error @+1 {{trait impl names '"Rc"', but 'Drop' is the only modeled trait}}
emitrust.impl "Owner" {
  emitrust.func @drop(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner">>) {
    emitrust.return
  }
} {trait_name = "Rc"}

// -----

// A Drop impl holds exactly one function and it is named `drop`: rustc
// E0407 ("method is not a member of trait Drop") is what a differently
// named member would become, so the verifier pins the shape here instead.
emitrust.impl "Owner" {
  // expected-error @+1 {{'Drop' impl member must be named 'drop'}}
  emitrust.func @cleanup(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner">>) {
    emitrust.return
  }
} {trait_name = "Drop"}

// -----

// `Drop::drop` returns nothing; a result would render a signature rustc
// rejects (E0053).
emitrust.impl "Owner" {
  // expected-error @+1 {{'Drop' impl member 'drop' must have no results}}
  emitrust.func @drop(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner">>) -> i32 {
    %0 = emitrust.constant <0 : i32> : i32
    emitrust.return %0 : i32
  }
} {trait_name = "Drop"}

// -----

// `Drop::drop` takes `&mut self` and nothing else.
emitrust.impl "Owner" {
  // expected-error @+1 {{'Drop' impl member 'drop' must take exactly one argument, the &mut self receiver}}
  emitrust.func @drop(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner">>, %arg1: i32) {
    emitrust.return
  }
} {trait_name = "Drop"}

// -----

// A receiverless associated function cannot be a Drop impl member: the
// W2.2 static-method escape hatch is explicitly closed here.
emitrust.impl "Owner" {
  // expected-error @+1 {{'Drop' impl member 'drop' must take exactly one argument, the &mut self receiver}}
  emitrust.func @drop() attributes {emitrust.static_method} {
    emitrust.return
  }
} {trait_name = "Drop"}

// -----

// A shared `&self` receiver is accepted by the INHERENT impl (W2.2 const
// methods) but not by a Drop impl: `Drop::drop` is `&mut self`.
emitrust.impl "Owner" {
  // expected-error @+1 {{'Drop' impl member 'drop' receiver must be a !emitrust.mut_ref of !emitrust.struct<"Owner">}}
  emitrust.func @drop(%arg0: !emitrust.ref<!emitrust.struct<"Owner">>) {
    emitrust.return
  }
} {trait_name = "Drop"}

// -----

// A Drop impl holds exactly ONE function. A second member would render
// inside `impl Drop for Owner`, where rustc has no trait item for it
// (E0407). (The empty-body counterpart is unrepresentable in textual IR:
// `emitrust.impl`'s single region must have a block.)
// expected-error @+1 {{'Drop' impl must hold exactly one emitrust.func}}
emitrust.impl "Owner" {
  emitrust.func @drop(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner">>) {
    emitrust.return
  }
  emitrust.func @also(%arg1: !emitrust.mut_ref<!emitrust.struct<"Owner">>) {
    emitrust.return
  }
} {trait_name = "Drop"}

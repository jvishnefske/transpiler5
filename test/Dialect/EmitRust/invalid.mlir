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
  // expected-error @+1 {{must appear inside an emitrust.loop or emitrust.for}}
  emitrust.break
  emitrust.return
}

// -----

emitrust.func @continue_outside_loop(%arg0: i1) {
  emitrust.if %arg0 {
    // expected-error @+1 {{must appear inside an emitrust.loop or emitrust.for}}
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
  // expected-error @+1 {{operand must be an lvalue of !emitrust.array or !emitrust.slice type}}
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

// expected-error @+1 {{duplicate variant value 0}}
emitrust.enum_def @DupValue ["Red", "Green"] [0, 0]

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
  // expected-error @+1 {{base must be an lvalue of !emitrust.array or !emitrust.slice type}}
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

// expected-error @+1 {{invalid fn_ptr parameter type '!emitrust.ref<i32>'}}
emitrust.func @fn_ptr_bad_component(%arg0: !emitrust.fn_ptr<(!emitrust.ref<i32>) -> i32>) {
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
  // expected-error @+1 {{receiver must be an lvalue of !emitrust.struct type}}
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

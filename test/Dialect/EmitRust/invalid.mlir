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
  // expected-error @+1 {{operand must be an lvalue of !emitrust.array type}}
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

emitrust.func @cast_to_enum(%arg0: i32) {
  // expected-error @+1 {{cannot cast to an enum type}}
  %0 = emitrust.cast %arg0 : i32 to !emitrust.enum<"Color">
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

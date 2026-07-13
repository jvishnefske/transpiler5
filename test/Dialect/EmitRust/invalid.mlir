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

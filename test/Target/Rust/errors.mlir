// FR-9: Unsupported constructs fail translation with a located diagnostic, not silent bad output.
// RUN: not emitrust-translate --mlir-to-rust --split-input-file %s 2>&1 | FileCheck %s

// An unsupported (tensor) type must be diagnosed.
emitrust.func @unsupported_type() {
  // CHECK: cannot translate type
  %0 = emitrust.literal "x" : tensor<4xi32>
  emitrust.return
}

// -----

// An op from a foreign dialect must be diagnosed.
emitrust.func @unsupported_op(%arg0: i32) {
  // CHECK: unable to translate op
  %0 = builtin.unrealized_conversion_cast %arg0 : i32 to i64
  emitrust.return
}

// -----

// Rust has no `as bool` cast; a boolean-producing cast must be diagnosed.
emitrust.func @cast_to_bool(%arg0: i32) {
  // CHECK: cannot translate a cast to bool
  %0 = emitrust.cast %arg0 : i32 to i1
  emitrust.return
}

// -----

// FR-61c: a while condition op the FR-61d machinery cannot fold (here a
// multi-use value needing a statement binding) is a located error --
// statements cannot render inside a `while` head, and silently wrong
// code is never an option.
emitrust.func @unfoldable_while(%arg0: i32, %arg1: i32) -> i32 {
  %m = emitrust.let mut %arg0 : i32
  emitrust.while {
    // CHECK: condition op does not fold into the head expression
    %t = emitrust.add %m, %arg1 : i32
    %c = emitrust.cmp lt, %t, %t : (i32, i32) -> i1
    emitrust.condition %c
  } do {
    emitrust.yield
  }
  emitrust.return %m : i32
}

// -----

// FR-70 marker contract: a module reaching emission with an
// external-requirement GLOBAL still marked skipped
// emitrust-lower-external-requirements. Unlike a marked FUNCTION (body-less,
// so translation dies naturally), a declaration-only global is perfectly
// renderable -- as a DEFAULTED thread_local static, i.e. fabricated storage
// the C program never had. Silence here would be a miscompile, so the
// emitter refuses instead.
// CHECK: unlowered external-requirement global 'g_config': emitrust-lower-external-requirements must run before Rust emission
emitrust.global @g_config {emitrust.external_requirement} : i32

// -----

// FR-77 marker contract: a fn-ptr constant's `Some(<name>)` is OPAQUE text,
// not a SymbolUse, so nothing structural stops a planner change from
// emitting a module that spells out a function the module does not contain
// -- the rendered crate is then rustc E0425, a whole-crate loss the emitter
// could have refused. The backstop: any identifier-shaped `Some(<name>)` on
// a fn_ptr-typed global initializer must resolve to a function in the
// module. (Non-identifier spellings -- `Some(0i64)` option-cursors,
// `Some(f::<T>)` requirement rewrites -- are other contracts' and pass.)
// CHECK: dangling function pointer target 'helper': the module defines no function with that name
emitrust.global @TU0_CB <#emitrust.opaque<"Some(helper)">> : !emitrust.fn_ptr<(i32) -> i32>

// -----

// FR-77, the aggregate position: a fn-ptr TABLE's leaves are the same opaque
// spelling nested in an ArrayAttr, so the walk must recurse (the FR-52
// global-initializer check did not, which is exactly how a table would have
// slipped this net).
// CHECK: dangling function pointer target 'op_a': the module defines no function with that name
emitrust.global const @OPS <[#emitrust.opaque<"Some(op_a)">, #emitrust.opaque<"None">]> : !emitrust.array<2x!emitrust.fn_ptr<(i32) -> i32>>

// -----

// FR-77, the rvalue position: an `emitrust.constant` of fn_ptr type inside a
// body dangles the same way.
emitrust.func @use_missing() -> !emitrust.fn_ptr<(i32) -> i32> {
  // CHECK: dangling function pointer target 'missing': the module defines no function with that name
  %0 = emitrust.constant <#emitrust.opaque<"Some(missing)">> : !emitrust.fn_ptr<(i32) -> i32>
  emitrust.return %0 : !emitrust.fn_ptr<(i32) -> i32>
}

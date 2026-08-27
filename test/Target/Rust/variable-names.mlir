// FR-61e slice 1: an `emitrust.variable` carrying its C name (the `named`
// attribute, pre-mangled importer-side) binds under that spelling instead
// of vN. This test pins the emitter-side invariants: the `_`-prefix rule
// still applies to never-read named variables (including reads removed by
// the FR-61d drop pass), every emitted binding name -- named or vN -- is
// uniquified against everything already bound in the function (a collision
// appends `_1`, `_2`, ...; a shadowing re-`let` is never emitted), and the
// vN auto-namer skips numbers whose spelling a named local has claimed, so
// a C local literally named `v1` can never collide with a generated name.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// A read+written named variable binds `let mut <name>`.
// CHECK-LABEL: fn named_mut(v0: i32) -> i32 {
// CHECK-NEXT:    let mut count: i32 = 1;
// CHECK-NEXT:    count = v0;
// CHECK-NEXT:    count
// CHECK-NEXT:  }
emitrust.func @named_mut(%arg0: i32) -> i32 {
  %c = emitrust.variable named "count" <1 : i32> : !emitrust.lvalue<i32>
  emitrust.assign %c = %arg0 : !emitrust.lvalue<i32>
  %v = emitrust.load %c : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %v : i32
}

// A named variable whose only reader was dropped (FR-61d) is `_`-prefixed
// exactly like an anonymous one.
// CHECK-LABEL: fn unused_name() {
// CHECK-NEXT:    let _scratch: i32 = 0;
// CHECK-NEXT:  }
emitrust.func @unused_name() {
  %s = emitrust.variable named "scratch" : !emitrust.lvalue<i32>
  %v = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  emitrust.return
}

// A keyword-spelled C local arrives PRE-mangled in the attribute; the
// emitter uses it verbatim.
// CHECK-LABEL: fn keyword(v0: i32) -> i32 {
// CHECK-NEXT:    let match_: i32 = 2;
// CHECK-NEXT:    match_ + v0
// CHECK-NEXT:  }
emitrust.func @keyword(%arg0: i32) -> i32 {
  %m = emitrust.variable named "match_" <2 : i32> : !emitrust.lvalue<i32>
  %v = emitrust.load %m : (!emitrust.lvalue<i32>) -> i32
  %s = emitrust.add %v, %arg0 : i32
  emitrust.return %s : i32
}

// A C local literally named `v1` claims the spelling; the auto-namer skips
// past it (the call binding becomes v0, the next generated number is v2 --
// consumed here by the inlined load).
// CHECK-LABEL: fn counter_skip() -> i32 {
// CHECK-NEXT:    let v1: i32 = 0;
// CHECK-NEXT:    let v0: i32 = get();
// CHECK-NEXT:    v0 + v1
// CHECK-NEXT:  }
emitrust.func @counter_skip() -> i32 {
  %a = emitrust.variable named "v1" : !emitrust.lvalue<i32>
  %r = emitrust.call_opaque "get"() : () -> i32
  %v = emitrust.load %a : (!emitrust.lvalue<i32>) -> i32
  %s = emitrust.add %r, %v : i32
  emitrust.return %s : i32
}

// Two same-named locals in one function: the second uniquifies to `x_1`;
// no shadowing re-`let` is ever emitted. (Both bindings defer their dead
// synthesized defaults -- the FR-61b deferral machinery names through the
// same path -- and FR-132 then merges each declaration with its initializing
// write. Names are claimed function-uniquely, so sinking a declaration can
// never turn the second binding into a shadow of the first: `x_1` stays
// `x_1`.)
// CHECK-LABEL: fn same_name(v0: i32) -> i32 {
// CHECK-NEXT:    let x: i32 = v0;
// CHECK-NEXT:    let x_1: i32 = v0;
// CHECK-NEXT:    x + x_1
// CHECK-NEXT:  }
emitrust.func @same_name(%arg0: i32) -> i32 {
  %x1 = emitrust.variable named "x" : !emitrust.lvalue<i32>
  %x2 = emitrust.variable named "x" : !emitrust.lvalue<i32>
  emitrust.assign %x1 = %arg0 : !emitrust.lvalue<i32>
  emitrust.assign %x2 = %arg0 : !emitrust.lvalue<i32>
  %a = emitrust.load %x1 : (!emitrust.lvalue<i32>) -> i32
  %b = emitrust.load %x2 : (!emitrust.lvalue<i32>) -> i32
  %s = emitrust.add %a, %b : i32
  emitrust.return %s : i32
}

// FR-61e slice 3: a plain signed scalar local stays on the alloca -> mem2reg
// SSA path (no `emitrust.variable`), so it carries its C name as a `NameLoc`
// wrapped around the promoted init value's location. `assignName` reads the
// `NameLoc` right after the `VariableOp` c_name check and binds under that
// spelling through the same collision-safe `claimName` -- so a multi-use
// scalar (a surviving `let` binding) is named, exactly like a carried
// variable, without any `emitrust.variable` op.
// CHECK-LABEL: fn scalar_carrier(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    let area: i32 = v0 * v1;
// CHECK-NEXT:    area + area
// CHECK-NEXT:  }
emitrust.func @scalar_carrier(%arg0: i32, %arg1: i32) -> i32 {
  %a = emitrust.mul %arg0, %arg1 : i32 loc("area")
  %s = emitrust.add %a, %a : i32
  emitrust.return %s : i32
}

// A single-use scalar is inlined by FR-61d, so its binding never exists and
// the carried `NameLoc` is simply never consumed -- "only surviving bindings"
// get named. The value renders inline with no `let` and no name.
// CHECK-LABEL: fn scalar_carrier_single_use(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    v0 * v1
// CHECK-NEXT:  }
emitrust.func @scalar_carrier_single_use(%arg0: i32, %arg1: i32) -> i32 {
  %a = emitrust.mul %arg0, %arg1 : i32 loc("temp")
  emitrust.return %a : i32
}

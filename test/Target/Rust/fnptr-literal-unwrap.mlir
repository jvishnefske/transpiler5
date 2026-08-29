// FR-133 (clippy::unnecessary_literal_unwrap): a fn-ptr LOCAL initialized from
// a LITERAL `Some(f)` needs no `Option` at all. This pins the drop of the
// wrapper -- `let cp: fn(i32) -> i32 = addc;` with a plain `cp(x)` call -- and,
// far more importantly, pins every REFUSAL leg, because dropping the wrapper
// where a `None` can still reach the binding is a miscompile that compiles.
//
// The admission fence is the three conditions of the FR, spelled here as one
// USE-SET rule over the `emitrust.variable`: the binding's only uses are
//   * exactly ONE whole-binding `emitrust.assign` whose value is an
//     `emitrust.constant` carrying the identifier-shaped `Some(<fn>)` opaque
//     text AND whose result feeds nothing else, and
//   * `emitrust.load`s whose every use is the CALLEE (operand 0) of an
//     `emitrust.call_indirect`.
// Reading the fence off the use set is what makes each of the FR's conditions
// checkable rather than assumed:
//   * "initialized from a literal Some" -- the assign's value must be that
//     constant; anything else (a parameter, another load, `None`) refuses;
//   * "never reassigned" -- a SECOND assign refuses, even when it is also a
//     literal `Some`. The multi-arm all-literal shape (fn-pointers.c's
//     `select_op`) is therefore deliberately OUT OF SCOPE: it is a
//     reassignment by this rule, its value escapes through a `return` anyway,
//     and admitting it would need definite-assignment reasoning the rendering
//     fold does not have;
//   * "never compared against null" -- a load feeding an `emitrust.cmp`
//     (which is how `fp == 0` / `if (!fp)` arrive) is not a call callee, so it
//     refuses. The `Option` is exactly the thing the comparison needs.
// A load that escapes (a call ARGUMENT, a `return`, a store into a struct
// field) refuses for the same reason: the consuming position is typed
// `Option<fn(..)>` and an unwrapped value would not fit it.
//
// The declaration must additionally be a DEFERRED binding. Every other
// rendering of a fn-ptr variable emits the synthesized `None` default
// (`emitDefaultValue`), which has no spelling once the `Option` is gone --
// see @unread_default below, whose `let _v0: Option<fn(i32) -> i32> = None;`
// must survive untouched.
//
// This is a RENDERING change only: no MLIR type changes, so `call_indirect`
// still verifies against `!emitrust.fn_ptr` and the round-trip is unaffected.
// A vNN that shifts in any golden is a bug, not churn; no CHECK may be
// relaxed to absorb one.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s --strict-whitespace

// ---------------------------------------------------------------------------
// The win.
// ---------------------------------------------------------------------------

// THE ADMITTED SHAPE: one literal `Some`, one call through it. Both the
// constant's binding and the local lose the wrapper, and the call loses its
// `.expect`.
// CHECK-LABEL: fn admitted(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: fn(i32) -> i32 = addc;
// CHECK-NEXT:    let cp: fn(i32) -> i32 = v1;
// CHECK-NEXT:    cp(v0)
// CHECK-NEXT:  }
emitrust.func @admitted(%arg0: i32) -> i32 {
  %cp = emitrust.variable named "cp"
      : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %s = emitrust.constant <#emitrust.opaque<"Some(addc)">>
      : !emitrust.fn_ptr<(i32) -> i32>
  emitrust.assign %cp = %s : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %l = emitrust.load %cp
      : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>)
      -> !emitrust.fn_ptr<(i32) -> i32>
  %r = emitrust.call_indirect %l(%arg0)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  emitrust.return %r : i32
}

// Two calls through the same local: every load is unwrapped, not just the
// first, and neither call keeps an `.expect`.
// CHECK-LABEL: fn admitted_two_calls(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: fn(i32) -> i32 = addc;
// CHECK-NEXT:    let cp: fn(i32) -> i32 = v1;
// CHECK-NEXT:    let v3: i32 = cp(v0);
// CHECK-NEXT:    cp(v3)
// CHECK-NEXT:  }
emitrust.func @admitted_two_calls(%arg0: i32) -> i32 {
  %cp = emitrust.variable named "cp"
      : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %s = emitrust.constant <#emitrust.opaque<"Some(addc)">>
      : !emitrust.fn_ptr<(i32) -> i32>
  emitrust.assign %cp = %s : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %l0 = emitrust.load %cp
      : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>)
      -> !emitrust.fn_ptr<(i32) -> i32>
  %r0 = emitrust.call_indirect %l0(%arg0)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  %l1 = emitrust.load %cp
      : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>)
      -> !emitrust.fn_ptr<(i32) -> i32>
  %r1 = emitrust.call_indirect %l1(%r0)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  emitrust.return %r1 : i32
}

// A void-result target: the `-> R` clause is absent from the unwrapped
// spelling exactly as it is from the wrapped one.
// CHECK-LABEL: fn admitted_void() {
// CHECK-NEXT:    let v0: fn() = tick;
// CHECK-NEXT:    let cb: fn() = v0;
// CHECK-NEXT:    cb();
// CHECK-NEXT:  }
emitrust.func @admitted_void() {
  %cp = emitrust.variable named "cb"
      : !emitrust.lvalue<!emitrust.fn_ptr<()>>
  %s = emitrust.constant <#emitrust.opaque<"Some(tick)">>
      : !emitrust.fn_ptr<()>
  emitrust.assign %cp = %s : !emitrust.lvalue<!emitrust.fn_ptr<()>>
  %l = emitrust.load %cp
      : (!emitrust.lvalue<!emitrust.fn_ptr<()>>) -> !emitrust.fn_ptr<()>
  emitrust.call_indirect %l() : (!emitrust.fn_ptr<()>) -> ()
  emitrust.return
}

// ---------------------------------------------------------------------------
// The refusals. Each keeps its `Option` AND its `.expect`.
// ---------------------------------------------------------------------------

// REASSIGNED to a different function. A second write could just as well have
// been `None`, so the wrapper stays; this is the condition that stops the
// fold from being a use-before-`None` miscompile.
// CHECK-LABEL: fn reassigned(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: Option<fn(i32) -> i32> = Some(addc);
// CHECK-NEXT:    let mut cp: Option<fn(i32) -> i32> = v1;
// CHECK-NEXT:    let v3: i32 = cp.expect("null function pointer")(v0);
// CHECK-NEXT:    let v4: Option<fn(i32) -> i32> = Some(subc);
// CHECK-NEXT:    cp = v4;
// CHECK-NEXT:    cp.expect("null function pointer")(v3)
// CHECK-NEXT:  }
emitrust.func @reassigned(%arg0: i32) -> i32 {
  %cp = emitrust.variable named "cp"
      : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %s0 = emitrust.constant <#emitrust.opaque<"Some(addc)">>
      : !emitrust.fn_ptr<(i32) -> i32>
  emitrust.assign %cp = %s0 : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %l0 = emitrust.load %cp
      : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>)
      -> !emitrust.fn_ptr<(i32) -> i32>
  %r0 = emitrust.call_indirect %l0(%arg0)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  %s1 = emitrust.constant <#emitrust.opaque<"Some(subc)">>
      : !emitrust.fn_ptr<(i32) -> i32>
  emitrust.assign %cp = %s1 : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %l1 = emitrust.load %cp
      : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>)
      -> !emitrust.fn_ptr<(i32) -> i32>
  %r1 = emitrust.call_indirect %l1(%r0)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  emitrust.return %r1 : i32
}

// COMPARED AGAINST NULL. The null test is the whole reason the `Option` is
// there; the load feeding an `emitrust.cmp` is not a call callee and refuses.
// CHECK-LABEL: fn null_compared(_v0: i32) -> bool {
// CHECK-NEXT:    let v1: Option<fn(i32) -> i32> = Some(addc);
// CHECK-NEXT:    let cp: Option<fn(i32) -> i32> = v1;
// CHECK-NEXT:    let v2: Option<fn(i32) -> i32> = cp;
// CHECK-NEXT:    v2.is_none()
// CHECK-NEXT:  }
emitrust.func @null_compared(%arg0: i32) -> i1 {
  %cp = emitrust.variable named "cp"
      : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %s = emitrust.constant <#emitrust.opaque<"Some(addc)">>
      : !emitrust.fn_ptr<(i32) -> i32>
  emitrust.assign %cp = %s : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %l = emitrust.load %cp
      : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>)
      -> !emitrust.fn_ptr<(i32) -> i32>
  %n = emitrust.constant <#emitrust.opaque<"None">>
      : !emitrust.fn_ptr<(i32) -> i32>
  %c = emitrust.cmp eq, %l, %n
      : (!emitrust.fn_ptr<(i32) -> i32>, !emitrust.fn_ptr<(i32) -> i32>) -> i1
  emitrust.return %c : i1
}

// MULTI-ARM, ALL LITERAL: `Some(addc)` on one arm and `Some(subc)` on the
// other. Two assigns, so out of scope by the "never reassigned" rule -- the
// deliberate conservative choice recorded in the header.
// CHECK-LABEL: fn multi_arm(v0: i32, v1: bool) -> i32 {
// CHECK-NEXT:    let cp: Option<fn(i32) -> i32> = if v1 {
// CHECK-NEXT:        let v2: Option<fn(i32) -> i32> = Some(addc);
// CHECK-NEXT:        v2
// CHECK-NEXT:    } else {
// CHECK-NEXT:        let v3: Option<fn(i32) -> i32> = Some(subc);
// CHECK-NEXT:        v3
// CHECK-NEXT:    };
// CHECK-NEXT:    cp.expect("null function pointer")(v0)
// CHECK-NEXT:  }
emitrust.func @multi_arm(%arg0: i32, %arg1: i1) -> i32 {
  %cp = emitrust.variable named "cp"
      : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  emitrust.if %arg1 {
    %s0 = emitrust.constant <#emitrust.opaque<"Some(addc)">>
        : !emitrust.fn_ptr<(i32) -> i32>
    emitrust.assign %cp = %s0
        : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
    emitrust.yield
  } else {
    %s1 = emitrust.constant <#emitrust.opaque<"Some(subc)">>
        : !emitrust.fn_ptr<(i32) -> i32>
    emitrust.assign %cp = %s1
        : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
    emitrust.yield
  }
  %l = emitrust.load %cp
      : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>)
      -> !emitrust.fn_ptr<(i32) -> i32>
  %r = emitrust.call_indirect %l(%arg0)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  emitrust.return %r : i32
}

// GENUINELY NULLABLE: initialized `None`, conditionally overwritten with a
// literal `Some`. The binding really can be `None` at the call, so the
// `.expect` is the C program's undefined behavior refined into a panic and
// must stay.
// CHECK-LABEL: fn may_be_none(v0: i32, v1: bool) -> i32 {
// CHECK-NEXT:    let v2: Option<fn(i32) -> i32> = None;
// CHECK-NEXT:    let mut cp: Option<fn(i32) -> i32> = v2;
// CHECK-NEXT:    if v1 {
// CHECK-NEXT:        let v3: Option<fn(i32) -> i32> = Some(addc);
// CHECK-NEXT:        cp = v3;
// CHECK-NEXT:    }
// CHECK-NEXT:    cp.expect("null function pointer")(v0)
// CHECK-NEXT:  }
emitrust.func @may_be_none(%arg0: i32, %arg1: i1) -> i32 {
  %cp = emitrust.variable named "cp"
      : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %n = emitrust.constant <#emitrust.opaque<"None">>
      : !emitrust.fn_ptr<(i32) -> i32>
  emitrust.assign %cp = %n : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  emitrust.if %arg1 {
    %s0 = emitrust.constant <#emitrust.opaque<"Some(addc)">>
        : !emitrust.fn_ptr<(i32) -> i32>
    emitrust.assign %cp = %s0
        : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
    emitrust.yield
  }
  %l = emitrust.load %cp
      : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>)
      -> !emitrust.fn_ptr<(i32) -> i32>
  %r = emitrust.call_indirect %l(%arg0)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  emitrust.return %r : i32
}

// ESCAPES as a call ARGUMENT: the callee's parameter is typed
// `Option<fn(i32) -> i32>`, so an unwrapped value would not fit. The
// operand-number test (callee is operand 0) is what separates this from the
// admitted shape.
// CHECK-LABEL: fn escapes_as_argument(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: Option<fn(i32) -> i32> = Some(addc);
// CHECK-NEXT:    let cp: Option<fn(i32) -> i32> = v1;
// CHECK-NEXT:    takes(cp, v0)
// CHECK-NEXT:  }
emitrust.func @escapes_as_argument(%arg0: i32) -> i32 {
  %cp = emitrust.variable named "cp"
      : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %s = emitrust.constant <#emitrust.opaque<"Some(addc)">>
      : !emitrust.fn_ptr<(i32) -> i32>
  emitrust.assign %cp = %s : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %l = emitrust.load %cp
      : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>)
      -> !emitrust.fn_ptr<(i32) -> i32>
  %r = emitrust.call_opaque "takes"(%l, %arg0)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  emitrust.return %r : i32
}

// ESCAPES through a `return`: the function result is `Option<fn(i32) -> i32>`.
// CHECK-LABEL: fn escapes_as_result() -> Option<fn(i32) -> i32> {
// CHECK-NEXT:    let v0: Option<fn(i32) -> i32> = Some(addc);
// CHECK-NEXT:    let cp: Option<fn(i32) -> i32> = v0;
// CHECK-NEXT:    cp
// CHECK-NEXT:  }
emitrust.func @escapes_as_result() -> !emitrust.fn_ptr<(i32) -> i32> {
  %cp = emitrust.variable named "cp"
      : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %s = emitrust.constant <#emitrust.opaque<"Some(addc)">>
      : !emitrust.fn_ptr<(i32) -> i32>
  emitrust.assign %cp = %s : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %l = emitrust.load %cp
      : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>)
      -> !emitrust.fn_ptr<(i32) -> i32>
  emitrust.return %l : !emitrust.fn_ptr<(i32) -> i32>
}

// ---------------------------------------------------------------------------
// LOCALS ONLY. A fn-ptr PARAMETER and a fn-ptr STRUCT MEMBER are outside this
// FR entirely: both are typed by a signature/definition this rendering fold
// cannot see, so neither may lose its `Option`.
// ---------------------------------------------------------------------------

// CHECK:      struct Calc {
// CHECK-NEXT:     op: Option<fn(i32) -> i32>,
// CHECK-NEXT:  }
emitrust.struct_def @Calc ["op"] [!emitrust.fn_ptr<(i32) -> i32>]

// CHECK-LABEL: fn via_parameter(v0: Option<fn(i32) -> i32>, v1: i32) -> i32 {
// CHECK-NEXT:    v0.expect("null function pointer")(v1)
// CHECK-NEXT:  }
emitrust.func @via_parameter(%arg0: !emitrust.fn_ptr<(i32) -> i32>,
                             %arg1: i32) -> i32 {
  %r = emitrust.call_indirect %arg0(%arg1)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  emitrust.return %r : i32
}

// CHECK-LABEL: fn via_member(v0: i32) -> i32 {
// CHECK-NEXT:    let mut c: Calc = Calc::default();
// CHECK-NEXT:    let v1: Option<fn(i32) -> i32> = Some(addc);
// CHECK-NEXT:    c.op = v1;
// CHECK-NEXT:    c.op.expect("null function pointer")(v0)
// CHECK-NEXT:  }
emitrust.func @via_member(%arg0: i32) -> i32 {
  %c = emitrust.variable named "c" : !emitrust.lvalue<!emitrust.struct<"Calc">>
  %m = emitrust.member %c["op"]
      : (!emitrust.lvalue<!emitrust.struct<"Calc">>)
      -> !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %s = emitrust.constant <#emitrust.opaque<"Some(addc)">>
      : !emitrust.fn_ptr<(i32) -> i32>
  emitrust.assign %m = %s : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %l = emitrust.load %m
      : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>)
      -> !emitrust.fn_ptr<(i32) -> i32>
  %r = emitrust.call_indirect %l(%arg0)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  emitrust.return %r : i32
}

// A fn-ptr variable that is never read is not a deferred binding: it renders
// its synthesized `None` default, which the unwrapped spelling has no way to
// write. It must keep the `Option`.
// CHECK-LABEL: fn unread_default() {
// CHECK-NEXT:    let _v0: Option<fn(i32) -> i32> = None;
// CHECK-NEXT:  }
emitrust.func @unread_default() {
  %cp = emitrust.variable : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %s = emitrust.constant <#emitrust.opaque<"Some(addc)">>
      : !emitrust.fn_ptr<(i32) -> i32>
  emitrust.assign %cp = %s : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  emitrust.return
}

// FR-77: every `Some(<name>)` above must name a function this module actually
// contains. Placed last so every pinned rendering above is byte-unchanged.
emitrust.func @addc(%arg0: i32) -> i32 {
  %c = emitrust.constant <1 : i32> : i32
  %r = emitrust.add %arg0, %c : i32
  emitrust.return %r : i32
}
emitrust.func @subc(%arg0: i32) -> i32 {
  %c = emitrust.constant <1 : i32> : i32
  %r = emitrust.sub %arg0, %c : i32
  emitrust.return %r : i32
}
emitrust.func @tick() {
  emitrust.return
}
emitrust.func @takes(%arg0: !emitrust.fn_ptr<(i32) -> i32>, %arg1: i32) -> i32 {
  %r = emitrust.call_indirect %arg0(%arg1)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  emitrust.return %r : i32
}

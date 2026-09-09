// FR-215 (clippy::needless_late_init, the residue FR-213 classified): FR-61b's
// IF-EXPRESSION BINDING generalised in two directions, pinned on hand-written
// IR so every refusal leg is reachable without a C source that happens to
// produce it.
//
//   1. SWITCH-EXPR -- `emitrust.switch` joins `emitrust.if` as a foldable
//      cond op. `let x: T; match d { 0 => { x = a; } _ => { x = b; } }` becomes
//      `let x: T = match d { 0 => { a } _ => { b } };`. Sound because
//      `emitrust.switch` carries a MANDATORY default region (ODS
//      `SizedRegion<1>`), so the emitted `match` is exhaustive and the binding
//      is initialized on every path -- the same property `if`+`else` has, and
//      the reason a bare `if` with no else has always refused.
//   2. SUNK -- the cond op no longer has to be `getNextNode()`. The gap is
//      SCANNED with FR-132's rule, not skipped: any op that MENTIONS the
//      binding refuses, a diverging op refuses, and the drop-order gate
//      refuses a may-drop declaration sunk past another may-drop value.
//      Without this the corpus's commonest shape is invisible, because an
//      inlined `emitrust.cmp` (`if argc > 1`) or an inlined cast chain (a
//      switch discriminator) SITS BETWEEN the declaration and the cond op in
//      the IR while rendering nothing at all.
//   3. DIVERGING ARM (`emitrust.if` only) -- one arm tail-assigns, the other
//      ends in `panic!`/`return`/`break`. Rust types a block whose last
//      statement diverges as `!`, which coerces to the binding's type;
//      verified with a standalone rustc probe over all five shapes the
//      emitter can produce, before this was written, rather than assumed.
//
// WHY THIS IS STILL A RENDERING CHANGE and not the forbidden liveness
// reasoning: `computeDeferredInits` has already proved nothing reads the
// binding before its first write, and the declaration emits NO CODE. What
// moves is where that nothing is written. No store is dropped, no expression
// moves, no evaluation order changes.
//
// The declaration's name is claimed even when it renders nothing (exactly as
// FR-132 does), so v-numbering is unchanged corpus-wide. If a vNN in any
// golden shifts, that unconditional claim was lost -- a bug, not churn.
//
// The refusal legs carry the correctness and each is pinned below: a switch
// arm that does not tail-assign, a switch arm that DIVERGES (a `break` ladder
// arm must never become a match arm's tail value), a second assignment
// anywhere in the function, a gap that mentions the binding, a gap that
// diverges, and the drop-order gate -- the last with a scalar twin of
// byte-identical IR proving the GATE and not the fold is what refuses.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

emitrust.struct_def @L ["id"] [i32] {emitrust.has_drop}

// ---------------------------------------------------------------------------
// 1. SWITCH-EXPR: the win.
// ---------------------------------------------------------------------------

// Every arm, including the mandatory default, tail-assigns the binding once.
// CHECK-LABEL: fn switch_expr(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    let s: i32 = match v0 {
// CHECK-NEXT:        0 => {
// CHECK-NEXT:            v1
// CHECK-NEXT:        }
// CHECK-NEXT:        7 => {
// CHECK-NEXT:            v2
// CHECK-NEXT:        }
// CHECK-NEXT:        _ => {
// CHECK-NEXT:            v0
// CHECK-NEXT:        }
// CHECK-NEXT:    };
// CHECK-NEXT:    s + s
// CHECK-NEXT:  }
emitrust.func @switch_expr(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
  emitrust.switch %arg0 : i32
  case 0 {
    emitrust.assign %s = %arg1 : !emitrust.lvalue<i32>
  }
  case 7 {
    emitrust.assign %s = %arg2 : !emitrust.lvalue<i32>
  }
  default {
    emitrust.assign %s = %arg0 : !emitrust.lvalue<i32>
  }
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %r2 = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %sum = emitrust.add %r, %r2 : i32
  emitrust.return %sum : i32
}

// THE TAIL FOLD (FR-61d slice 3, generalised). Without it the fold merely
// TRADES `needless_late_init` for `clippy::let_and_return`: the corpus shape
// is `let v9: i32; match .. { .. } v9`, and folding only the first half leaves
// `let v9: i32 = match .. {}; v9`. The `let` prefix must disappear entirely
// and the `match` must become the function's tail expression -- note there is
// NO trailing semicolon on the closing brace.
// CHECK-LABEL: fn switch_expr_tail(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    match v0 {
// CHECK-NEXT:        0 => {
// CHECK-NEXT:            v1
// CHECK-NEXT:        }
// CHECK-NEXT:        _ => {
// CHECK-NEXT:            v0
// CHECK-NEXT:        }
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @switch_expr_tail(%arg0: i32, %arg1: i32) -> i32 {
  %zero = emitrust.constant <0 : i32> : i32
  %s = emitrust.let mut %zero : i32
  emitrust.switch %arg0 : i32
  case 0 {
    emitrust.assign %s = %arg1 : i32
  }
  default {
    emitrust.assign %s = %arg0 : i32
  }
  emitrust.return %s : i32
}

// An arm may carry statements before its tail assignment; only the LAST
// emitted op has to be the write. The arm-local statement still renders inside
// the arm, so the printing side effect keeps its position.
// CHECK-LABEL: fn switch_expr_arm_body(v0: i32) -> i32 {
// CHECK-NEXT:    let s: i32 = match v0 {
// CHECK-NEXT:        3 => {
// CHECK-NEXT:            println!("three");
// CHECK-NEXT:            let v1: i32 = side(v0);
// CHECK-NEXT:            v1
// CHECK-NEXT:        }
// CHECK-NEXT:        _ => {
// CHECK-NEXT:            0i32
// CHECK-NEXT:        }
// CHECK-NEXT:    };
// CHECK-NEXT:    s + s
// CHECK-NEXT:  }
emitrust.func @switch_expr_arm_body(%arg0: i32) -> i32 {
  %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
  emitrust.switch %arg0 : i32
  case 3 {
    emitrust.call_opaque "println!"() {args = ["three"]} : () -> ()
    %c = emitrust.call_opaque "side"(%arg0) : (i32) -> i32
    emitrust.assign %s = %c : !emitrust.lvalue<i32>
  }
  default {
    %z = emitrust.constant <0 : i32> : i32
    emitrust.assign %s = %z : !emitrust.lvalue<i32>
  }
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %r2 = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %sum = emitrust.add %r, %r2 : i32
  emitrust.return %sum : i32
}

// ---------------------------------------------------------------------------
// 1r. SWITCH-EXPR: the refusals.
// ---------------------------------------------------------------------------

// A DIVERGING ARM REFUSES. `emitrust.break` inside a case is transparent with
// respect to the enclosing loop, so that arm never reaches an assignment and
// the binding is NOT initialized on that path. The declaration below IS a
// deferred one (the break path never reads it, so the dead default still
// drops), so what refuses here is `armTailAssign` stopping at the first
// diverging op -- not `computeDeferredInits` upstream of it. Admitting it
// would have to invent a value for the `break` arm.
// CHECK-LABEL: fn switch_break_arm(v0: i32, v1: i32) {
// CHECK-NEXT:    loop {
// CHECK-NEXT:        let s: i32;
// CHECK-NEXT:        match v0 {
// CHECK-NEXT:            0 => {
// CHECK-NEXT:                break;
// CHECK-NEXT:            }
// CHECK-NEXT:            _ => {
// CHECK-NEXT:                s = v1;
// CHECK-NEXT:            }
// CHECK-NEXT:        }
// CHECK-NEXT:        println!("{}", s);
// CHECK-NEXT:        break;
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @switch_break_arm(%arg0: i32, %arg1: i32) {
  emitrust.loop {
    %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
    emitrust.switch %arg0 : i32
    case 0 {
      emitrust.break
    }
    default {
      emitrust.assign %s = %arg1 : !emitrust.lvalue<i32>
    }
    %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
    emitrust.call_opaque "println!"(%r) {args = ["{}", 0 : index]} : (i32) -> ()
    emitrust.break
  }
  emitrust.return
}

// An arm that assigns nothing at all refuses for the same reason -- there is
// no value to be that arm's tail. Shaped like the leg above so the binding is
// deferred and the refusal is attributable to the arm scan.
// CHECK-LABEL: fn switch_silent_arm(v0: i32, v1: i32) {
// CHECK-NEXT:    loop {
// CHECK-NEXT:        let s: i32;
// CHECK-NEXT:        match v0 {
// CHECK-NEXT:            0 => {
// CHECK-NEXT:                println!("nothing");
// CHECK-NEXT:                break;
// CHECK-NEXT:            }
// CHECK-NEXT:            _ => {
// CHECK-NEXT:                s = v1;
// CHECK-NEXT:            }
// CHECK-NEXT:        }
// CHECK-NEXT:        println!("{}", s);
// CHECK-NEXT:        break;
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @switch_silent_arm(%arg0: i32, %arg1: i32) {
  emitrust.loop {
    %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
    emitrust.switch %arg0 : i32
    case 0 {
      emitrust.call_opaque "println!"() {args = ["nothing"]} : () -> ()
      emitrust.break
    }
    default {
      emitrust.assign %s = %arg1 : !emitrust.lvalue<i32>
    }
    %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
    emitrust.call_opaque "println!"(%r) {args = ["{}", 0 : index]} : (i32) -> ()
    emitrust.break
  }
  emitrust.return
}

// A THIRD assignment outside the switch refuses: the arms are no longer the
// binding's only writes, and folding would drop one.
// CHECK-LABEL: fn switch_extra_assign(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    let mut s: i32;
// CHECK-NEXT:    match v0 {
// CHECK-NEXT:        0 => {
// CHECK-NEXT:            s = v1;
// CHECK-NEXT:        }
// CHECK-NEXT:        _ => {
// CHECK-NEXT:            s = v0;
// CHECK-NEXT:        }
// CHECK-NEXT:    }
// CHECK-NEXT:    println!("mid {}", s);
// CHECK-NEXT:    s = v1;
// CHECK-NEXT:    s
// CHECK-NEXT:  }
emitrust.func @switch_extra_assign(%arg0: i32, %arg1: i32) -> i32 {
  %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
  emitrust.switch %arg0 : i32
  case 0 {
    emitrust.assign %s = %arg1 : !emitrust.lvalue<i32>
  }
  default {
    emitrust.assign %s = %arg0 : !emitrust.lvalue<i32>
  }
  %m = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "println!"(%m) {args = ["mid {}", 0 : index]} : (i32) -> ()
  emitrust.assign %s = %arg1 : !emitrust.lvalue<i32>
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// ---------------------------------------------------------------------------
// 2. SUNK: the win, and the gap rule.
// ---------------------------------------------------------------------------

// A statement between the declaration and the `if`. In the corpus this is
// usually an inlined `emitrust.cmp` that renders nothing, but a gap op that
// DOES render is the same question and is pinned here: it keeps its position,
// and the declaration disappears into the if-expression below it.
// CHECK-LABEL: fn if_expr_sunk(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    let v3: bool = wide(v0);
// CHECK-NEXT:    let s: i32 = if v3 {
// CHECK-NEXT:        v1
// CHECK-NEXT:    } else {
// CHECK-NEXT:        v2
// CHECK-NEXT:    };
// CHECK-NEXT:    s + s
// CHECK-NEXT:  }
emitrust.func @if_expr_sunk(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
  %c = emitrust.call_opaque "wide"(%arg0) : (i32) -> i1
  emitrust.if %c {
    emitrust.assign %s = %arg1 : !emitrust.lvalue<i32>
  } else {
    emitrust.assign %s = %arg2 : !emitrust.lvalue<i32>
  }
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %r2 = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %sum = emitrust.add %r, %r2 : i32
  emitrust.return %sum : i32
}

// The same relaxation for a switch: the discriminator's cast sits between the
// declaration and the `emitrust.switch`.
// CHECK-LABEL: fn switch_expr_sunk(v0: i64, v1: i32) -> i32 {
// CHECK-NEXT:    let v2: i32 = v0 as i32;
// CHECK-NEXT:    let s: i32 = match v2 {
// CHECK-NEXT:        1 => {
// CHECK-NEXT:            v1
// CHECK-NEXT:        }
// CHECK-NEXT:        _ => {
// CHECK-NEXT:            v2
// CHECK-NEXT:        }
// CHECK-NEXT:    };
// CHECK-NEXT:    s + v2
// CHECK-NEXT:  }
emitrust.func @switch_expr_sunk(%arg0: i64, %arg1: i32) -> i32 {
  %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
  %d = emitrust.cast %arg0 : i64 to i32
  emitrust.switch %d : i32
  case 1 {
    emitrust.assign %s = %arg1 : !emitrust.lvalue<i32>
  }
  default {
    emitrust.assign %s = %d : !emitrust.lvalue<i32>
  }
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %sum = emitrust.add %r, %d : i32
  emitrust.return %sum : i32
}

// ANY MENTION OF THE BINDING IN THE GAP REFUSES -- FR-132's rule, imported
// verbatim. It is DEFENCE IN DEPTH here and the leg says so rather than
// overclaiming: every mention this fold could meet is independently refused
// upstream (a read makes the binding non-deferrable -- rustc E0381 -- and a
// second write fails the arms-are-the-only-assignments test), so the gap scan
// is the guard that would still hold if either of those moved. The shape
// below is the borrow: `s` keeps its initializer AND does not fold.
// CHECK-LABEL: fn sunk_gap_mentions(v0: bool, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    let mut s: i32 = 0;
// CHECK-NEXT:    let v3: &mut i32 = &mut s;
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:        s = v1;
// CHECK-NEXT:    } else {
// CHECK-NEXT:        s = v2;
// CHECK-NEXT:    }
// CHECK-NEXT:    take(v3);
// CHECK-NEXT:    s
// CHECK-NEXT:  }
emitrust.func @sunk_gap_mentions(%arg0: i1, %arg1: i32, %arg2: i32) -> i32 {
  %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
  %p = emitrust.addr_of mut %s : (!emitrust.lvalue<i32>) -> !emitrust.mut_ref<i32>
  emitrust.if %arg0 {
    emitrust.assign %s = %arg1 : !emitrust.lvalue<i32>
  } else {
    emitrust.assign %s = %arg2 : !emitrust.lvalue<i32>
  }
  emitrust.call_opaque "take"(%p) : (!emitrust.mut_ref<i32>) -> ()
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// DROP ORDER, the soundness pin, carried over from FR-132 verbatim. `b` is a
// may-drop declaration still owning at the `if`; sinking `a`'s declaration
// past it flips two destructors, and BOTH orderings compile clean, so only
// this pin catches it.
// CHECK-LABEL: fn sunk_drop_order(v0: bool) {
// CHECK-NEXT:    let a: L;
// CHECK-NEXT:    let b: L = L::default();
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:        let v3: L = mk(1i32);
// CHECK-NEXT:        a = v3;
// CHECK-NEXT:    } else {
// CHECK-NEXT:        let v4: L = mk(2i32);
// CHECK-NEXT:        a = v4;
// CHECK-NEXT:    }
// CHECK-NEXT:    sink(b);
// CHECK-NEXT:    sink(a);
// CHECK-NEXT:  }
emitrust.func @sunk_drop_order(%arg0: i1) {
  %a = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.struct<"L">>
  %b = emitrust.variable named "b" : !emitrust.lvalue<!emitrust.struct<"L">>
  %one = emitrust.constant <1 : i32> : i32
  %two = emitrust.constant <2 : i32> : i32
  emitrust.if %arg0 {
    %m1 = emitrust.call_opaque "mk"(%one) : (i32) -> !emitrust.struct<"L">
    emitrust.assign %a = %m1 : !emitrust.lvalue<!emitrust.struct<"L">>
  } else {
    %m2 = emitrust.call_opaque "mk"(%two) : (i32) -> !emitrust.struct<"L">
    emitrust.assign %a = %m2 : !emitrust.lvalue<!emitrust.struct<"L">>
  }
  %lb = emitrust.load %b : (!emitrust.lvalue<!emitrust.struct<"L">>) -> !emitrust.struct<"L">
  emitrust.call_opaque "sink"(%lb) : (!emitrust.struct<"L">) -> ()
  %la = emitrust.load %a : (!emitrust.lvalue<!emitrust.struct<"L">>) -> !emitrust.struct<"L">
  emitrust.call_opaque "sink"(%la) : (!emitrust.struct<"L">) -> ()
  emitrust.return
}

// THE SCALAR TWIN: byte-identical IR but for the type, so what refuses above
// is the DROP-ORDER GATE and not the sunk gap itself. `i32` cannot drop, so
// this one folds.
// CHECK-LABEL: fn sunk_drop_order_scalar_twin(v0: bool) {
// CHECK-NEXT:    let b: i32 = 0;
// CHECK-NEXT:    let a: i32 = if v0 {
// CHECK-NEXT:        let v3: i32 = mk(1i32);
// CHECK-NEXT:        v3
// CHECK-NEXT:    } else {
// CHECK-NEXT:        let v4: i32 = mk(2i32);
// CHECK-NEXT:        v4
// CHECK-NEXT:    };
// CHECK-NEXT:    sink(b);
// CHECK-NEXT:    sink(a);
// CHECK-NEXT:  }
emitrust.func @sunk_drop_order_scalar_twin(%arg0: i1) {
  %a = emitrust.variable named "a" : !emitrust.lvalue<i32>
  %b = emitrust.variable named "b" : !emitrust.lvalue<i32>
  %one = emitrust.constant <1 : i32> : i32
  %two = emitrust.constant <2 : i32> : i32
  emitrust.if %arg0 {
    %m1 = emitrust.call_opaque "mk"(%one) : (i32) -> i32
    emitrust.assign %a = %m1 : !emitrust.lvalue<i32>
  } else {
    %m2 = emitrust.call_opaque "mk"(%two) : (i32) -> i32
    emitrust.assign %a = %m2 : !emitrust.lvalue<i32>
  }
  %lb = emitrust.load %b : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "sink"(%lb) : (i32) -> ()
  %la = emitrust.load %a : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "sink"(%la) : (i32) -> ()
  emitrust.return
}

// ---------------------------------------------------------------------------
// 3. DIVERGING IF ARM.
// ---------------------------------------------------------------------------

// One arm tail-assigns, the other panics. The panicking arm keeps its
// STATEMENT form (trailing `;`), which is what rustc's divergence analysis
// needs to give the block type `!`; the coercion was verified with a rustc
// probe over every shape the emitter produces here.
// CHECK-LABEL: fn if_diverge_else(v0: bool, v1: i32) -> i32 {
// CHECK-NEXT:    let s: i32 = if v0 {
// CHECK-NEXT:        v1
// CHECK-NEXT:    } else {
// CHECK-NEXT:        panic!("no fixed argument");
// CHECK-NEXT:    };
// CHECK-NEXT:    s + s
// CHECK-NEXT:  }
emitrust.func @if_diverge_else(%arg0: i1, %arg1: i32) -> i32 {
  %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
  emitrust.if %arg0 {
    emitrust.assign %s = %arg1 : !emitrust.lvalue<i32>
  } else {
    emitrust.call_opaque "panic!"() {args = ["no fixed argument"]} : () -> ()
  }
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %r2 = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %sum = emitrust.add %r, %r2 : i32
  emitrust.return %sum : i32
}

// The mirror image: the THEN arm diverges, with a statement before the panic
// that keeps its position inside the arm.
// CHECK-LABEL: fn if_diverge_then(v0: bool, v1: i32) -> i32 {
// CHECK-NEXT:    let s: i32 = if v0 {
// CHECK-NEXT:        println!("bad");
// CHECK-NEXT:        panic!("no fixed argument");
// CHECK-NEXT:    } else {
// CHECK-NEXT:        v1
// CHECK-NEXT:    };
// CHECK-NEXT:    s + s
// CHECK-NEXT:  }
emitrust.func @if_diverge_then(%arg0: i1, %arg1: i32) -> i32 {
  %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
  emitrust.if %arg0 {
    emitrust.call_opaque "println!"() {args = ["bad"]} : () -> ()
    emitrust.call_opaque "panic!"() {args = ["no fixed argument"]} : () -> ()
  } else {
    emitrust.assign %s = %arg1 : !emitrust.lvalue<i32>
  }
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %r2 = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %sum = emitrust.add %r, %r2 : i32
  emitrust.return %sum : i32
}

// BOTH ARMS DIVERGING REFUSES: there is no value at all, so there is nothing
// to bind. The binding here IS deferred and IS initialized -- by a write after
// the `if` -- so the guard that fires is `armAssigns.empty()`, and the fold
// that then claims the declaration is FR-132's late-init merge, one program
// point later. Admitting the empty case would have produced
// `let s: i32 = if c { panic!(..) } else { panic!(..) };` and DROPPED the real
// initializer below it.
// CHECK-LABEL: fn if_diverge_both(v0: bool, v1: i32) -> i32 {
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:        panic!("a");
// CHECK-NEXT:    } else {
// CHECK-NEXT:        panic!("b");
// CHECK-NEXT:    }
// CHECK-NEXT:    let s: i32 = v1;
// CHECK-NEXT:    s + s
// CHECK-NEXT:  }
emitrust.func @if_diverge_both(%arg0: i1, %arg1: i32) -> i32 {
  %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
  emitrust.if %arg0 {
    emitrust.call_opaque "panic!"() {args = ["a"]} : () -> ()
  } else {
    emitrust.call_opaque "panic!"() {args = ["b"]} : () -> ()
  }
  emitrust.assign %s = %arg1 : !emitrust.lvalue<i32>
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %r2 = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %sum = emitrust.add %r, %r2 : i32
  emitrust.return %sum : i32
}

// A STRUCTURED-TERMINATOR arm folds too: `let x: T = if c { v } else { break; };`
// is legal Rust and the `break` keeps its exact semantics. (`emitrust.return`
// is a terminator constrained to `emitrust.func`, so `break` is the reachable
// non-panic diverging arm; the `return` spelling was checked against rustc in
// the same probe and is admitted by the same `opDiverges` test.)
// CHECK-LABEL: fn if_diverge_break(v0: bool, v1: i32) {
// CHECK-NEXT:    loop {
// CHECK-NEXT:        let s: i32 = if v0 {
// CHECK-NEXT:            v1
// CHECK-NEXT:        } else {
// CHECK-NEXT:            break;
// CHECK-NEXT:        };
// CHECK-NEXT:        println!("{}", s);
// CHECK-NEXT:        break;
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @if_diverge_break(%arg0: i1, %arg1: i32) {
  emitrust.loop {
    %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
    emitrust.if %arg0 {
      emitrust.assign %s = %arg1 : !emitrust.lvalue<i32>
    } else {
      emitrust.break
    }
    %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
    emitrust.call_opaque "println!"(%r) {args = ["{}", 0 : index]} : (i32) -> ()
    emitrust.break
  }
  emitrust.return
}

// A bare `if` with NO else still refuses, unchanged: the fall-through path
// leaves the binding uninitialized and there is no arm to give it a value. As
// with the gap leg above this is layered -- such a binding is not deferrable
// either -- and the `getElseRegion().empty()` test is what would still refuse
// if it were.
// CHECK-LABEL: fn if_no_else(v0: bool, v1: i32) -> i32 {
// CHECK-NEXT:    let mut s: i32 = 0;
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:        s = v1;
// CHECK-NEXT:    }
// CHECK-NEXT:    s
// CHECK-NEXT:  }
emitrust.func @if_no_else(%arg0: i1, %arg1: i32) -> i32 {
  %s = emitrust.variable named "s" : !emitrust.lvalue<i32>
  emitrust.if %arg0 {
    emitrust.assign %s = %arg1 : !emitrust.lvalue<i32>
  }
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// FR-130 increment 2: DEAD-BINDING ELIMINATION. The SCF lowering destroys SSA
// for irreducible control flow by materializing a default-initialized
// `let mut` per carried value and writing it in every arm; when nothing ever
// READS the binding, the emitter already knows it (that is why the name is
// `_`-prefixed) but used to keep the whole `let` plus every write, because
// dropping the binding alone would orphan the writes (E0425). This pins that
// such a binding now disappears TOGETHER with all of its writes.
//
// Why this is not the forbidden cross-iteration dead-store reasoning: no
// liveness is computed at all. `valueIsRead` classifies an assignment
// DESTINATION as a write, never a read, so a `false` answer means the binding
// is read NOWHERE, on NO path, in NO region -- every write to it is trivially
// dead. That is the maximal form of "PROVES no read on any path".
//
// The three refusal legs matter more than the win, because dropping an
// assignment whose right-hand side has an EFFECT is a silent miscompile that
// no rustc lint can see (`deny(unused_variables)`/`deny(unused_assignments)`
// are blind: the binding is already `_`-prefixed). The gate is
// refuse-by-default -- a right-hand side must be a bare name (block argument)
// or an effect-free producer -- and a single refusing write keeps the WHOLE
// binding, writes included: no partial drops.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// THE WIN: `%dead` is written in both arms (a block-argument name in one, a
// constant in the other -- the only two right-hand-side shapes the emitted
// corpus actually produces) and never read. Binding and both writes go. The
// CHECK-NEXT chain is the pin: nothing survives between the `let` of the live
// binding and the tail expression.
// CHECK-LABEL: fn dead_binding(v0: i32) -> i32 {
// CHECK-NEXT:    let [[L:v[0-9]+]]: i32;
// CHECK-NEXT:    if v0 > 0i32 {
// CHECK-NEXT:        [[L]] = 1i32;
// CHECK-NEXT:    } else {
// CHECK-NEXT:        [[L]] = 2i32;
// CHECK-NEXT:    }
// CHECK-NEXT:    [[L]]
// CHECK-NEXT:  }
emitrust.func @dead_binding(%arg0: i32) -> i32 {
  %zero = emitrust.constant <0 : i32> : i32
  %dead = emitrust.let mut %zero : i32
  %z2 = emitrust.constant <0 : i32> : i32
  %live = emitrust.let mut %z2 : i32
  %c = emitrust.cmp gt, %arg0, %zero : (i32, i32) -> i1
  emitrust.if %c {
    %one = emitrust.constant <1 : i32> : i32
    emitrust.assign %live = %one : i32
    emitrust.assign %dead = %arg0 : i32
    emitrust.yield
  } else {
    %two = emitrust.constant <2 : i32> : i32
    emitrust.assign %live = %two : i32
    emitrust.assign %dead = %two : i32
    emitrust.yield
  }
  emitrust.return %live : i32
}

// REFUSAL 1 -- an impure WRITE. `%dead` is never read here either, but one of
// its writes carries a call result. Everything stays: the binding, the call
// site, BOTH writes (including the pure `2i32` one in the else arm -- no
// partial drops). Note the call renders its own `let` statement rather than
// folding into the assignment, which is exactly why refusing on the
// right-hand side's DEFINING op is the correct place to look.
// CHECK-LABEL: fn impure_write(v0: i32) -> i32 {
// CHECK-NEXT:    let mut [[D:_v[0-9]+]]: i32 = 0i32;
// CHECK-NEXT:    let [[L2:v[0-9]+]]: i32;
// CHECK-NEXT:    if v0 > 0i32 {
// CHECK-NEXT:        [[L2]] = 1i32;
// CHECK-NEXT:        let [[C:v[0-9]+]]: i32 = bump(v0);
// CHECK-NEXT:        [[D]] = [[C]];
// CHECK-NEXT:    } else {
// CHECK-NEXT:        [[L2]] = 2i32;
// CHECK-NEXT:        [[D]] = 2i32;
// CHECK-NEXT:    }
// CHECK-NEXT:    [[L2]]
// CHECK-NEXT:  }
emitrust.func @impure_write(%arg0: i32) -> i32 {
  %zero = emitrust.constant <0 : i32> : i32
  %dead = emitrust.let mut %zero : i32
  %z2 = emitrust.constant <0 : i32> : i32
  %live = emitrust.let mut %z2 : i32
  %c = emitrust.cmp gt, %arg0, %zero : (i32, i32) -> i1
  emitrust.if %c {
    %one = emitrust.constant <1 : i32> : i32
    emitrust.assign %live = %one : i32
    %call = emitrust.call_opaque "bump"(%arg0) : (i32) -> i32
    emitrust.assign %dead = %call : i32
    emitrust.yield
  } else {
    %two = emitrust.constant <2 : i32> : i32
    emitrust.assign %live = %two : i32
    emitrust.assign %dead = %two : i32
    emitrust.yield
  }
  emitrust.return %live : i32
}

// REFUSAL 2 -- an impure INITIALIZER. The binding's own init operand vanishes
// with the `let`, so it is gated exactly like a write. Here every WRITE is
// pure and the binding is still never read, yet the call in the initializer
// position keeps everything.
// CHECK-LABEL: fn impure_init(v0: i32) -> i32 {
// CHECK-NEXT:    let [[C2:v[0-9]+]]: i32 = bump(v0);
// CHECK-NEXT:    let mut [[D2:_v[0-9]+]]: i32 = [[C2]];
// CHECK-NEXT:    let [[L3:v[0-9]+]]: i32;
// CHECK-NEXT:    if v0 > 0i32 {
// CHECK-NEXT:        [[L3]] = 1i32;
// CHECK-NEXT:        [[D2]] = v0;
// CHECK-NEXT:    } else {
// CHECK-NEXT:        [[L3]] = 2i32;
// CHECK-NEXT:        [[D2]] = 2i32;
// CHECK-NEXT:    }
// CHECK-NEXT:    [[L3]]
// CHECK-NEXT:  }
emitrust.func @impure_init(%arg0: i32) -> i32 {
  %init = emitrust.call_opaque "bump"(%arg0) : (i32) -> i32
  %dead = emitrust.let mut %init : i32
  %zero = emitrust.constant <0 : i32> : i32
  %z2 = emitrust.constant <0 : i32> : i32
  %live = emitrust.let mut %z2 : i32
  %c = emitrust.cmp gt, %arg0, %zero : (i32, i32) -> i1
  emitrust.if %c {
    %one = emitrust.constant <1 : i32> : i32
    emitrust.assign %live = %one : i32
    emitrust.assign %dead = %arg0 : i32
    emitrust.yield
  } else {
    %two = emitrust.constant <2 : i32> : i32
    emitrust.assign %live = %two : i32
    emitrust.assign %dead = %two : i32
    emitrust.yield
  }
  emitrust.return %live : i32
}

// REFUSAL 3 (W2.17) -- a DESTRUCTOR-carrying binding is observable even when
// never read: the `let` runs `D::drop` at end of scope and the assignment runs
// it on the overwritten value. Both writes here are bare names, so only the
// `has_drop` gate stands between this shape and a deleted destructor.
emitrust.struct_def @D ["id"] [i32] {emitrust.has_drop}
// TWO whole writes, so the W2.23 deferral carve-out does not apply and the
// binding really does render `let mut .. = ..;` plus both assignments: the
// `has_drop` gate is the ONLY thing between this shape and three deleted
// destructor runs, since every right-hand side here is a bare parameter name
// and would otherwise sail through the purity gate.
// CHECK-LABEL: fn drop_binding(v0: D, v1: D, v2: D) {
// CHECK-NEXT:    let mut [[DD:_v[0-9]+]]: D = v0;
// CHECK-NEXT:    [[DD]] = v1;
// CHECK-NEXT:    [[DD]] = v2;
// CHECK-NEXT:  }
emitrust.func @drop_binding(%arg0: !emitrust.struct<"D">, %arg1: !emitrust.struct<"D">, %arg2: !emitrust.struct<"D">) {
  %dead = emitrust.let mut %arg0 : !emitrust.struct<"D">
  emitrust.assign %dead = %arg1 : !emitrust.struct<"D">
  emitrust.assign %dead = %arg2 : !emitrust.struct<"D">
  emitrust.return
}

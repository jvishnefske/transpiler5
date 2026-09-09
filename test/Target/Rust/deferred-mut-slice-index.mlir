// FR-142 (defect): a DEFERRED binding used as the INDEX of a mutable slice
// borrow was emitted `let mut` and never reassigned, so the emitted crate --
// which denies `unused_mut` -- did not compile at all:
// `error: variable does not need to be mutable` on `let mut v7: i64;`.
// Exit 0, unbuildable, no diagnostic.
//
// The cause is a missing operand-identity test. `emitrust.slice_of` has TWO
// operands, `$base` (an lvalue) and `$index`, and `binding.getUsers()` returns
// the op through EITHER. `computeDeferredInits`'s post-init-mutation scan
// scored `sliceOf.getIsMut()` without asking WHICH operand the binding is, so
// `&mut place[binding as usize..]` -- where the binding is a pure READ serving
// as the start index -- was counted as a mutable borrow OF THE BINDING. The
// rule was already written verbatim in that loop's own comment ("only when the
// binding is the refined BASE, not a subscript index"), and the adjacent
// `MethodCallOp` branch already asks `call.getReceiver() == binding`.
//
// This file pins BOTH legs of that distinction in one place, because the
// failure mode of the fix is over-relaxation:
//   * INDEX  -> the mutable borrow is of some OTHER place; the binding is read,
//               so the `mut` must go (@index_no_lift, @index_lifts_to_if_expr).
//   * BASE   -> the mutable borrow IS of the binding, after its single
//               initializing write; the `mut` must STAY (@base_keeps_mut).
// Both directions are hard build failures in an emitted crate -- a missing
// `mut` is rustc E0384 and a stale one is the `unused_mut` deny -- so neither
// arm here is decoration.
//
// SECOND-ORDER EFFECT, pinned deliberately by @index_lifts_to_if_expr: with
// the spurious `mut` cleared, FR-61b's cond-expression fold becomes eligible
// and lifts the both-arms-assign-once shape into a `let v = if c { .. } else
// { .. };` expression binding. That is a change in EMITTED BYTES and it is
// correct.
//
// MOVED BY FR-215, deliberately, and the invariant is unchanged. @index_no_lift
// was the same shape with one statement between the declaration and the `if`,
// and it was out of the lift's reach ONLY because FR-61b required
// `op->getNextNode()`. FR-215 replaced that adjacency test with FR-132's scope
// rule -- a gap of statements that cannot observe the binding no longer blocks
// the fold -- so that leg now lifts too and is renamed @index_gap_lift. The
// FR-142 `mut` decision it existed to pin is NOT weakened by the move: the
// fold's own admission test is `deferredInits`' needsMut flag, so if the
// spurious `mut` ever came back the fold would REFUSE and this CHECK block
// would fail on the statement-form rendering. The two-program-point spelling
// of that same decision is still pinned byte-for-byte by @index_two_point_mut
// below, which FR-215 refuses for an independent reason (a diverging arm).
//
// Note on `lvalueIsMutated`: it carries the identical unguarded `slice_of`
// shape and CANNOT misfire, because its `value` is always an lvalue and an
// lvalue can never be a `slice_of` INDEX (the index is `AnyInteger|Index`).
// Latent, not live -- and that is why the index legs below must be spelled
// with SSA `emitrust.let` bindings: no lvalue binding can reach the defect.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// ---------------------------------------------------------------------------
// INDEX: the defect. The `mut` must go.
// ---------------------------------------------------------------------------

// The binding is written once per arm and then READ as the slice start index.
// A statement sits between the declaration and the `if`; under FR-215's scope
// rule that gap no longer blocks the lift, so this renders as an if-expression
// binding -- WITHOUT `mut`, which is the FR-142 invariant.
// CHECK-LABEL: fn index_gap_lift(v0: bool, v1: &mut [i32], v2: i64) -> i64 {
// CHECK-NEXT:    let _v5: i64 = barrier(v2);
// CHECK-NEXT:    let v4: i64 = if v0 {
// CHECK-NEXT:      v2
// CHECK-NEXT:    } else {
// CHECK-NEXT:      0i64
// CHECK-NEXT:    };
// CHECK-NEXT:    let _v6: &mut [i32] = &mut (*v1)[v4 as usize..];
// CHECK-NEXT:    v4
// CHECK-NEXT:  }
emitrust.func @index_gap_lift(%c: i1, %arr: !emitrust.mut_ref<!emitrust.slice<i32>>, %x: i64) -> i64 {
  %z = emitrust.constant <0 : i64> : i64
  %k = emitrust.let mut %z : i64
  %b = emitrust.call_opaque "barrier"(%x) : (i64) -> i64
  emitrust.if %c {
    emitrust.assign %k = %x : i64
  } else {
    emitrust.assign %k = %z : i64
  }
  %s = emitrust.deref %arr : (!emitrust.mut_ref<!emitrust.slice<i32>>) -> !emitrust.lvalue<!emitrust.slice<i32>>
  %m = emitrust.slice_of mut %s[%k] : (!emitrust.lvalue<!emitrust.slice<i32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  emitrust.return %k : i64
}

// THE TWO-PROGRAM-POINT SPELLING of the same `mut` decision, kept isolated
// after FR-215 moved the leg above. The `switch`'s first arm DIVERGES, which
// FR-215 refuses outright, so the declaration and its write stay two separate
// statements -- and the declaration must still read `let v4: i64;` and not
// `let mut v4: i64;`. A regression of FR-142 shows up here as a stale `mut`
// on a binding that is only ever a slice INDEX, which an emitted crate rejects
// outright (`deny(unused_mut)`).
// CHECK-LABEL: fn index_two_point_mut(v0: i32, v1: &mut [i32], v2: i64) {
// CHECK-NEXT:    loop {
// CHECK-NEXT:        let v4: i64;
// CHECK-NEXT:        match v0 {
// CHECK-NEXT:            0 => {
// CHECK-NEXT:                break;
// CHECK-NEXT:            }
// CHECK-NEXT:            _ => {
// CHECK-NEXT:                v4 = v2;
// CHECK-NEXT:            }
// CHECK-NEXT:        }
// CHECK-NEXT:        let _v5: &mut [i32] = &mut (*v1)[v4 as usize..];
// CHECK-NEXT:        break;
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @index_two_point_mut(%d: i32, %arr: !emitrust.mut_ref<!emitrust.slice<i32>>, %x: i64) {
  %z = emitrust.constant <0 : i64> : i64
  emitrust.loop {
    %k = emitrust.let mut %z : i64
    emitrust.switch %d : i32
    case 0 {
      emitrust.break
    }
    default {
      emitrust.assign %k = %x : i64
    }
    %s = emitrust.deref %arr : (!emitrust.mut_ref<!emitrust.slice<i32>>) -> !emitrust.lvalue<!emitrust.slice<i32>>
    %m = emitrust.slice_of mut %s[%k] : (!emitrust.lvalue<!emitrust.slice<i32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
    emitrust.break
  }
  emitrust.return
}

// The same shape with the `if` immediately following the declaration: clearing
// the spurious `mut` lets FR-61b lift the whole thing into an if-EXPRESSION
// binding. This is the corpus-visible byte change (sds), and it is correct.
// CHECK-LABEL: fn index_lifts_to_if_expr(v0: bool, v1: &mut [i32], v2: i64) -> i64 {
// CHECK-NEXT:    let v4: i64 = if v0 {
// CHECK-NEXT:      v2
// CHECK-NEXT:    } else {
// CHECK-NEXT:      0i64
// CHECK-NEXT:    };
// CHECK-NEXT:    let _v5: &mut [i32] = &mut (*v1)[v4 as usize..];
// CHECK-NEXT:    v4
// CHECK-NEXT:  }
emitrust.func @index_lifts_to_if_expr(%c: i1, %arr: !emitrust.mut_ref<!emitrust.slice<i32>>, %x: i64) -> i64 {
  %z = emitrust.constant <0 : i64> : i64
  %k = emitrust.let mut %z : i64
  emitrust.if %c {
    emitrust.assign %k = %x : i64
  } else {
    emitrust.assign %k = %z : i64
  }
  %s = emitrust.deref %arr : (!emitrust.mut_ref<!emitrust.slice<i32>>) -> !emitrust.lvalue<!emitrust.slice<i32>>
  %m = emitrust.slice_of mut %s[%k] : (!emitrust.lvalue<!emitrust.slice<i32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  emitrust.return %k : i64
}

// A SHARED slice borrow indexed by the binding: never a mutation in either
// reading of the code, so this arm was already correct and stays correct. It
// is the control for the two above.
// CHECK-LABEL: fn index_shared(v0: bool, v1: &mut [i32], v2: i64) -> i64 {
// CHECK-NEXT:    let v4: i64 = if v0 {
// CHECK-NEXT:      v2
// CHECK-NEXT:    } else {
// CHECK-NEXT:      0i64
// CHECK-NEXT:    };
// CHECK-NEXT:    let _v5: &[i32] = &(*v1)[v4 as usize..];
// CHECK-NEXT:    v4
// CHECK-NEXT:  }
emitrust.func @index_shared(%c: i1, %arr: !emitrust.mut_ref<!emitrust.slice<i32>>, %x: i64) -> i64 {
  %z = emitrust.constant <0 : i64> : i64
  %k = emitrust.let mut %z : i64
  emitrust.if %c {
    emitrust.assign %k = %x : i64
  } else {
    emitrust.assign %k = %z : i64
  }
  %s = emitrust.deref %arr : (!emitrust.mut_ref<!emitrust.slice<i32>>) -> !emitrust.lvalue<!emitrust.slice<i32>>
  %m = emitrust.slice_of %s[%k] : (!emitrust.lvalue<!emitrust.slice<i32>>, i64) -> !emitrust.ref<!emitrust.slice<i32>>
  emitrust.return %k : i64
}

// ---------------------------------------------------------------------------
// The over-relaxation guards. Each of these MUST keep its `mut`.
// ---------------------------------------------------------------------------

// BASE: the mutable slice borrow is OF THE BINDING, taken after its single
// initializing write. Dropping this `mut` emits `&mut v3[..]` against a
// non-`mut` `let` -- rustc E0596. This is the arm the fix must not reach.
// CHECK-LABEL: fn base_keeps_mut(v0: bool, v1: i64, v2: [i32; 4]) {
// CHECK-NEXT:    let mut v3: [i32; 4];
// CHECK-NEXT:    if v0 {
// CHECK:         let _v4: &mut [i32] = &mut v3[v1 as usize..];
// CHECK-NEXT:  }
emitrust.func @base_keeps_mut(%c: i1, %i: i64, %src: !emitrust.array<4xi32>) {
  %a = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  emitrust.if %c {
    emitrust.assign %a = %src : !emitrust.lvalue<!emitrust.array<4xi32>>
  } else {
    emitrust.assign %a = %src : !emitrust.lvalue<!emitrust.array<4xi32>>
  }
  %m = emitrust.slice_of mut %a[%i] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  emitrust.return
}

// BASE, shared: the twin of @base_keeps_mut with the `mut` marker removed from
// the borrow. It loses the `mut`, which proves the arm above is decided by the
// borrow's mutability and not by the base-ness alone.
// CHECK-LABEL: fn base_shared_no_mut(v0: bool, v1: i64, v2: [i32; 4]) {
// CHECK-NEXT:    let v3: [i32; 4] = if v0 {
// CHECK-NEXT:      v2
// CHECK-NEXT:    } else {
// CHECK-NEXT:      v2
// CHECK-NEXT:    };
// CHECK-NEXT:    let _v4: &[i32] = &v3[v1 as usize..];
// CHECK-NEXT:  }
emitrust.func @base_shared_no_mut(%c: i1, %i: i64, %src: !emitrust.array<4xi32>) {
  %a = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  emitrust.if %c {
    emitrust.assign %a = %src : !emitrust.lvalue<!emitrust.array<4xi32>>
  } else {
    emitrust.assign %a = %src : !emitrust.lvalue<!emitrust.array<4xi32>>
  }
  %m = emitrust.slice_of %a[%i] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.ref<!emitrust.slice<i32>>
  emitrust.return
}

// The binding is a slice INDEX *and* is genuinely reassigned afterwards. The
// `mut` comes from `maxWrites`/`loopReassign`, not from the borrow, and the
// narrowed borrow test must not disturb it: emitting this without `mut` is
// rustc E0384.
// CHECK-LABEL: fn index_but_reassigned(v0: bool, v1: &mut [i32], v2: i64) -> i64 {
// CHECK-NEXT:    let mut v4: i64;
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:      v4 = v2;
// CHECK-NEXT:    } else {
// CHECK-NEXT:      v4 = 0i64;
// CHECK-NEXT:    }
// CHECK-NEXT:    let _v5: &mut [i32] = &mut (*v1)[v4 as usize..];
// CHECK-NEXT:    v4 = 0i64;
// CHECK-NEXT:    v4
// CHECK-NEXT:  }
emitrust.func @index_but_reassigned(%c: i1, %arr: !emitrust.mut_ref<!emitrust.slice<i32>>, %x: i64) -> i64 {
  %z = emitrust.constant <0 : i64> : i64
  %k = emitrust.let mut %z : i64
  emitrust.if %c {
    emitrust.assign %k = %x : i64
  } else {
    emitrust.assign %k = %z : i64
  }
  %s = emitrust.deref %arr : (!emitrust.mut_ref<!emitrust.slice<i32>>) -> !emitrust.lvalue<!emitrust.slice<i32>>
  %m = emitrust.slice_of mut %s[%k] : (!emitrust.lvalue<!emitrust.slice<i32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  emitrust.assign %k = %z : i64
  emitrust.return %k : i64
}

// FR-63 (clippy::assign_op_pattern, SSA-binding residue): the lost-copy
// cycle breaker wraps every rotated back-edge value in a pass-through alias
// (`%t = emitrust.let %rhs`) before the backedge `emitrust.assign`, so the
// assign's RHS-defining op is the LetOp, not the binary op, and the original
// SSA-identity fold (`binOp->getOperand(0) == var`) never saw it. This file
// pins the two additive legs that close those residual sites:
//   1. ALIAS LOOK-THROUGH: when the target is an SSA `let mut` binding and
//      BOTH the alias and its initializer render inline in the assign (the
//      alias's own `let` statement never emits -- its text IS the
//      initializer's text), the fold sees through the alias chain:
//      `i = i + 1` shaped IR folds to `i += 1` even behind the alias.
//   2. COMMUTATIVE (operand 1): `a = e + a` folds to `a += e` when the op is
//      commutative with identical infix rendering on integers (+ * & | ^;
//      unsigned + * are excluded upstream by the wrapping-method rendering,
//      floats are excluded because NaN payload propagation is order-
//      sensitive) and `e` is provably pure to reorder against the read of
//      `a` (a name, a literal, or a pure load/cast chain -- anything the
//      purity walker cannot prove stays unfolded).
// The guards are load-bearing: `-` in operand-1 position must NOT fold
// (a = b - a is not a -= b), an unprovably-pure operand 0 must NOT fold, a
// non-inline (multi-use) alias must NOT fold (its `let` statement emits, so
// the assign renders only the alias name), distinct bindings never fold, and
// a zero-shift initializer behind an alias keeps today's identity rendering
// instead of resurrecting the dropped shift amount. Place targets are
// untouched (pinned by compound-assign-place.mlir / -projection.mlir).
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// Alias look-through, operand-0 identity: the rotations counter shape
// (`i = i + 1` behind the lost-copy alias) folds to `+=`.
// CHECK-LABEL: fn alias_op0(v0: i32) -> i32 {
// CHECK-NEXT:    let mut v2: i32 = 0i32;
// CHECK-NEXT:    while v2 < v0 {
// CHECK-NEXT:        v2 += 1i32;
// CHECK-NEXT:    }
// CHECK-NEXT:    v2
// CHECK-NEXT:  }
emitrust.func @alias_op0(%arg0: i32) -> i32 {
  %zero = emitrust.constant <0 : i32> : i32
  %i = emitrust.let mut %zero : i32
  emitrust.while {
    %c = emitrust.cmp lt, %i, %arg0 : (i32, i32) -> i1
    emitrust.condition %c
  } do {
    %one = emitrust.constant <1 : i32> : i32
    %next = emitrust.add %i, %one : i32
    %t = emitrust.let %next : i32
    emitrust.assign %i = %t : i32
    emitrust.yield
  }
  emitrust.return %i : i32
}

// Alias look-through + commutative operand-1: the fib3 rotation's
// `c = a + c` (target is operand ONE, operand 0 is another binding's pure
// name) folds to `c += a`. The addition's operand order flips, which is
// value-identical on integers.
// CHECK-LABEL: fn alias_commutative(v0: i32) -> i32 {
// CHECK-NEXT:    let mut v2: i32 = 1i32;
// CHECK-NEXT:    let mut v3: i32 = 1i32;
// CHECK-NEXT:    while v3 < v0 {
// CHECK-NEXT:        let v7: i32 = v3;
// CHECK-NEXT:        v3 += v2;
// CHECK-NEXT:        v2 = v7;
// CHECK-NEXT:    }
// CHECK-NEXT:    v3
// CHECK-NEXT:  }
emitrust.func @alias_commutative(%arg0: i32) -> i32 {
  %one = emitrust.constant <1 : i32> : i32
  %b = emitrust.let mut %one : i32
  %c = emitrust.let mut %one : i32
  emitrust.while {
    %cond = emitrust.cmp lt, %c, %arg0 : (i32, i32) -> i1
    emitrust.condition %cond
  } do {
    %sum = emitrust.add %b, %c : i32
    %t0 = emitrust.let %sum : i32
    %t1 = emitrust.let %c : i32
    emitrust.assign %c = %t0 : i32
    emitrust.assign %b = %t1 : i32
    emitrust.yield
  }
  emitrust.return %c : i32
}

// Commutative fold without an alias: `a = e + a` with the binary op feeding
// the assign directly still folds (the leg is independent of look-through).
// CHECK-LABEL: fn commutative_direct(v0: i32) -> i32 {
// CHECK-NEXT:    let mut v2: i32 = 3i32;
// CHECK-NEXT:    v2 += v0;
// CHECK-NEXT:    v2
// CHECK-NEXT:  }
emitrust.func @commutative_direct(%arg0: i32) -> i32 {
  %three = emitrust.constant <3 : i32> : i32
  %a = emitrust.let mut %three : i32
  %n = emitrust.add %arg0, %a : i32
  emitrust.assign %a = %n : i32
  emitrust.return %a : i32
}

// NON-fold: `-` in operand-1 position. `a = e - a` is NOT `a -= e`; the
// assign keeps its plain form.
// CHECK-LABEL: fn sub_operand1_stays(v0: i32) -> i32 {
// CHECK-NEXT:    let mut v2: i32 = 3i32;
// CHECK-NEXT:    v2 = v0 - v2;
// CHECK-NEXT:    v2
// CHECK-NEXT:  }
emitrust.func @sub_operand1_stays(%arg0: i32) -> i32 {
  %three = emitrust.constant <3 : i32> : i32
  %a = emitrust.let mut %three : i32
  %n = emitrust.sub %arg0, %a : i32
  emitrust.assign %a = %n : i32
  emitrust.return %a : i32
}

// NON-fold: operand 0 not provably pure to reorder against the read of the
// target -- an inline compound expression (here `v0 + v1`) is beyond the
// purity walker, so the assign keeps its plain form (unprovable => no fold).
// CHECK-LABEL: fn impure_operand0_stays(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    let mut v3: i32 = 3i32;
// CHECK-NEXT:    v3 = v0 + v1 + v3;
// CHECK-NEXT:    v3
// CHECK-NEXT:  }
emitrust.func @impure_operand0_stays(%arg0: i32, %arg1: i32) -> i32 {
  %three = emitrust.constant <3 : i32> : i32
  %a = emitrust.let mut %three : i32
  %e = emitrust.add %arg0, %arg1 : i32
  %n = emitrust.add %e, %a : i32
  emitrust.assign %a = %n : i32
  emitrust.return %a : i32
}

// NON-fold: distinct bindings. Neither operand is the target, so the assign
// keeps its plain form -- no `+=` (the SSA analogue of compound-assign-place's
// `notself`; the dead literal init is dropped, and FR-132 then merges the
// deferred declaration with its initializing write).
// CHECK-LABEL: fn distinct_stays(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    let v3: i32 = v0 + v1;
// CHECK-NEXT:    v3
// CHECK-NEXT:  }
emitrust.func @distinct_stays(%arg0: i32, %arg1: i32) -> i32 {
  %three = emitrust.constant <3 : i32> : i32
  %a = emitrust.let mut %three : i32
  %n = emitrust.add %arg0, %arg1 : i32
  emitrust.assign %a = %n : i32
  emitrust.return %a : i32
}

// NON-fold: a multi-use alias emits its own `let` statement, so the assign's
// text is only the alias NAME -- folding would drop a render that is not
// there and orphan nothing, but the fold must still refuse (the RHS binary
// op does not render in this statement).
// CHECK-LABEL: fn alias_multiuse_stays(_v0: i32) -> i32 {
// CHECK-NEXT:    let mut v2: i32 = 3i32;
// CHECK-NEXT:    let v5: i32 = v2 + 1i32;
// CHECK-NEXT:    v2 = v5;
// CHECK-NEXT:    v2 + v5
// CHECK-NEXT:  }
emitrust.func @alias_multiuse_stays(%arg0: i32) -> i32 {
  %three = emitrust.constant <3 : i32> : i32
  %a = emitrust.let mut %three : i32
  %one = emitrust.constant <1 : i32> : i32
  %n = emitrust.add %a, %one : i32
  %t = emitrust.let %n : i32
  emitrust.assign %a = %t : i32
  %r = emitrust.add %a, %t : i32
  emitrust.return %r : i32
}

// NON-fold: a zero-shift initializer behind an alias renders its lhs alone
// (`v = v;`, the FR-63 identity_op fold); look-through must refuse it, or
// the "fold" would resurrect the dropped shift amount as `v <<= 0`.
// CHECK-LABEL: fn alias_zero_shift_stays(_v0: i32) -> i32 {
// CHECK-NEXT:    let mut v2: i32 = 3i32;
// CHECK-NEXT:    v2 = v2;
// CHECK-NEXT:    v2
// CHECK-NEXT:  }
emitrust.func @alias_zero_shift_stays(%arg0: i32) -> i32 {
  %three = emitrust.constant <3 : i32> : i32
  %a = emitrust.let mut %three : i32
  %zero = emitrust.constant <0 : i32> : i32
  %n = emitrust.shl %a, %zero : i32
  %t = emitrust.let %n : i32
  emitrust.assign %a = %t : i32
  emitrust.return %a : i32
}

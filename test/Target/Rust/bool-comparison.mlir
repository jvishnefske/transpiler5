// FR-63 (clippy::bool_comparison): an eq/ne comparison against a literal
// bool constant renders as the idiomatic form instead of `x == false`. This
// test pins the fold's exact shapes and their precedence classification:
// (1) positive polarity ((eq,true)/(ne,false)) renders the non-constant
// operand's text alone, inheriting its precedence rank; (2) negative
// polarity ((eq,false)/(ne,true)) over a name renders `!name` at Unary
// rank; (3) negative polarity over an INLINED integer comparison renders
// the inner comparison with the exactly-inverted predicate (== <-> !=,
// < <-> >=, > <-> <=), staying at Compare rank; (4) the literal side may be
// lhs or rhs. The fold is SPELLING only -- it must never change what the
// program computes -- so the shapes it cannot render lint-free stay
// UN-folded and are pinned too: an inlined float ORDER comparison (NaN:
// !(a < b) is not a >= b), an inlined operand that is itself a bool-literal
// fold, and a two-literal comparison (a constant fold, out of scope).
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// `x == false` over a bare bool binding renders the negation, not the
// literal comparison.
// CHECK-LABEL: fn negate_bare(v0: bool) {
// CHECK-NEXT:    if !v0 {
// CHECK-NEXT:        sink();
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @negate_bare(%arg0: i1) {
  %f = emitrust.constant <false> : i1
  %r = emitrust.cmp eq, %arg0, %f : (i1, i1) -> i1
  emitrust.if %r {
    emitrust.call_opaque "sink"() : () -> ()
  }
  emitrust.return
}

// The literal side may be the LHS: `false == x` folds identically.
// CHECK-LABEL: fn negate_literal_lhs(v0: bool) {
// CHECK-NEXT:    if !v0 {
emitrust.func @negate_literal_lhs(%arg0: i1) {
  %f = emitrust.constant <false> : i1
  %r = emitrust.cmp eq, %f, %arg0 : (i1, i1) -> i1
  emitrust.if %r {
    emitrust.call_opaque "sink"() : () -> ()
  }
  emitrust.return
}

// `x != true` is the same negative polarity as `x == false`.
// CHECK-LABEL: fn negate_ne_true(v0: bool) {
// CHECK-NEXT:    if !v0 {
emitrust.func @negate_ne_true(%arg0: i1) {
  %t = emitrust.constant <true> : i1
  %r = emitrust.cmp ne, %arg0, %t : (i1, i1) -> i1
  emitrust.if %r {
    emitrust.call_opaque "sink"() : () -> ()
  }
  emitrust.return
}

// Positive polarity: `x == true` and `x != false` render the operand alone.
// CHECK-LABEL: fn identity_eq_true(v0: bool) {
// CHECK-NEXT:    if v0 {
emitrust.func @identity_eq_true(%arg0: i1) {
  %t = emitrust.constant <true> : i1
  %r = emitrust.cmp eq, %arg0, %t : (i1, i1) -> i1
  emitrust.if %r {
    emitrust.call_opaque "sink"() : () -> ()
  }
  emitrust.return
}

// CHECK-LABEL: fn identity_ne_false(v0: bool) {
// CHECK-NEXT:    if v0 {
emitrust.func @identity_ne_false(%arg0: i1) {
  %f = emitrust.constant <false> : i1
  %r = emitrust.cmp ne, %arg0, %f : (i1, i1) -> i1
  emitrust.if %r {
    emitrust.call_opaque "sink"() : () -> ()
  }
  emitrust.return
}

// Negative polarity over an INLINED integer comparison renders the inner
// comparison with the inverted predicate -- every predicate pair, exact
// over the total integer order: eq<->ne, lt<->ge, le<->gt.
// CHECK-LABEL: fn invert_int(v0: i32, v1: i32) {
// CHECK-NEXT:    sink(v0 != v1, v0 == v1, v0 >= v1, v0 > v1, v0 <= v1, v0 < v1);
emitrust.func @invert_int(%arg0: i32, %arg1: i32) {
  %f = emitrust.constant <false> : i1
  %eq = emitrust.cmp eq, %arg0, %arg1 : (i32, i32) -> i1
  %req = emitrust.cmp eq, %eq, %f : (i1, i1) -> i1
  %ne = emitrust.cmp ne, %arg0, %arg1 : (i32, i32) -> i1
  %rne = emitrust.cmp eq, %ne, %f : (i1, i1) -> i1
  %lt = emitrust.cmp lt, %arg0, %arg1 : (i32, i32) -> i1
  %rlt = emitrust.cmp eq, %lt, %f : (i1, i1) -> i1
  %le = emitrust.cmp le, %arg0, %arg1 : (i32, i32) -> i1
  %rle = emitrust.cmp eq, %le, %f : (i1, i1) -> i1
  %gt = emitrust.cmp gt, %arg0, %arg1 : (i32, i32) -> i1
  %rgt = emitrust.cmp eq, %gt, %f : (i1, i1) -> i1
  %ge = emitrust.cmp ge, %arg0, %arg1 : (i32, i32) -> i1
  %rge = emitrust.cmp eq, %ge, %f : (i1, i1) -> i1
  emitrust.call_opaque "sink"(%req, %rne, %rlt, %rle, %rgt, %rge)
      : (i1, i1, i1, i1, i1, i1) -> ()
  emitrust.return
}

// A float ORDER comparison must NOT invert (`!(a < b)` is not `a >= b`
// when a NaN is involved) and has no lint-free negated spelling, so the
// literal comparison stays -- the conservative non-fold.
// CHECK-LABEL: fn float_order_unfolded(v0: f64, v1: f64) {
// CHECK-NEXT:    if (v0 < v1) == false {
emitrust.func @float_order_unfolded(%arg0: f64, %arg1: f64) {
  %f = emitrust.constant <false> : i1
  %c = emitrust.cmp lt, %arg0, %arg1 : (f64, f64) -> i1
  %r = emitrust.cmp eq, %c, %f : (i1, i1) -> i1
  emitrust.if %r {
    emitrust.call_opaque "sink"() : () -> ()
  }
  emitrust.return
}

// Float eq/ne ARE complementary under IEEE (a NaN makes `==` false and
// `!=` true), so equality inverts even over floats.
// CHECK-LABEL: fn float_eq_inverts(v0: f64, v1: f64) {
// CHECK-NEXT:    if v0 != v1 {
emitrust.func @float_eq_inverts(%arg0: f64, %arg1: f64) {
  %f = emitrust.constant <false> : i1
  %c = emitrust.cmp eq, %arg0, %arg1 : (f64, f64) -> i1
  %r = emitrust.cmp eq, %c, %f : (i1, i1) -> i1
  emitrust.if %r {
    emitrust.call_opaque "sink"() : () -> ()
  }
  emitrust.return
}

// The fold is positional: a multi-use result keeps its `let` statement and
// the fold applies to the bound right-hand side.
// CHECK-LABEL: fn let_bound(v0: bool) -> bool {
// CHECK-NEXT:    let v2: bool = !v0;
// CHECK-NEXT:    sink(v2);
// CHECK-NEXT:    v2
// CHECK-NEXT:  }
emitrust.func @let_bound(%arg0: i1) -> i1 {
  %f = emitrust.constant <false> : i1
  %r = emitrust.cmp eq, %arg0, %f : (i1, i1) -> i1
  emitrust.call_opaque "sink"(%r) : (i1) -> ()
  emitrust.return %r : i1
}

// A comparison operand that renders by NAME (multi-use, so not inlined)
// negates the name even though its producer is a comparison -- inversion
// only applies to text that renders inline here.
// CHECK-LABEL: fn negate_named_inner(v0: i32, v1: i32) -> bool {
// CHECK-NEXT:    let v2: bool = v0 < v1;
// CHECK-NEXT:    sink(v2);
// CHECK-NEXT:    !v2
// CHECK-NEXT:  }
emitrust.func @negate_named_inner(%arg0: i32, %arg1: i32) -> i1 {
  %c = emitrust.cmp lt, %arg0, %arg1 : (i32, i32) -> i1
  emitrust.call_opaque "sink"(%c) : (i1) -> ()
  %f = emitrust.constant <false> : i1
  %r = emitrust.cmp eq, %c, %f : (i1, i1) -> i1
  emitrust.return %r : i1
}

// An inlined operand that is itself a bool-literal fold does not invert
// (its captured text is already the folded `!v0`, not an infix comparison);
// the outer comparison conservatively keeps the literal form. `!` binds
// tighter than `==`, so the bare text is exact.
// CHECK-LABEL: fn double_fold_unfolded(v0: bool) {
// CHECK-NEXT:    if !v0 == false {
emitrust.func @double_fold_unfolded(%arg0: i1) {
  %f = emitrust.constant <false> : i1
  %n = emitrust.cmp eq, %arg0, %f : (i1, i1) -> i1
  %r = emitrust.cmp eq, %n, %f : (i1, i1) -> i1
  emitrust.if %r {
    emitrust.call_opaque "sink"() : () -> ()
  }
  emitrust.return
}

// TWO literal operands are a constant-fold opportunity, not a spelling
// change: out of the fold's scope, rendering stays as-is.
// CHECK-LABEL: fn both_literal() {
// CHECK-NEXT:    sink(true == false);
emitrust.func @both_literal() {
  %t = emitrust.constant <true> : i1
  %f = emitrust.constant <false> : i1
  %r = emitrust.cmp eq, %t, %f : (i1, i1) -> i1
  emitrust.call_opaque "sink"(%r) : (i1) -> ()
  emitrust.return
}

// Precedence classification of the folded text. The negation is Unary:
// under a bitwise `&` (tighter than Compare, looser than Unary) it stays
// bare, exactly as `!` binds in Rust.
// CHECK-LABEL: fn negation_prec(v0: bool, v1: bool) -> bool {
// CHECK-NEXT:    !v0 & v1
// CHECK-NEXT:  }
emitrust.func @negation_prec(%arg0: i1, %arg1: i1) -> i1 {
  %f = emitrust.constant <false> : i1
  %n = emitrust.cmp eq, %arg0, %f : (i1, i1) -> i1
  %a = emitrust.and %n, %arg1 : i1
  emitrust.return %a : i1
}

// The identity fold inherits the operand's rank: an inner comparison stays
// Compare, so a consuming comparison (non-associative in Rust) must
// parenthesize it.
// CHECK-LABEL: fn identity_prec(v0: i32, v1: i32, v2: bool) {
// CHECK-NEXT:    sink((v0 < v1) == v2);
emitrust.func @identity_prec(%arg0: i32, %arg1: i32, %arg2: i1) {
  %c = emitrust.cmp lt, %arg0, %arg1 : (i32, i32) -> i1
  %t = emitrust.constant <true> : i1
  %r = emitrust.cmp eq, %c, %t : (i1, i1) -> i1
  %r2 = emitrust.cmp eq, %r, %arg2 : (i1, i1) -> i1
  emitrust.call_opaque "sink"(%r2) : (i1) -> ()
  emitrust.return
}

// FR-63 (clippy::identity_op): a shift whose amount is the literal integer
// 0 is the identity on every integer type (`x << 0` == `x >> 0` == `x`, and
// a zero amount is always below the width, so no panic path exists either),
// so the emitter renders the lhs ALONE. This test pins the fold's exact
// shape and, critically, its precedence classification: the rendered text
// is the lhs's, so the captured rank and trailing-cast flag must be
// INHERITED from the lhs -- a stale Shift-rank classification would drop
// needed parens in the `&`/`|` chains the bitfield lowering composes
// (`(a | b) << 0 & m` must keep `(a | b) & m`, never `a | b & m`). The fold
// is SPELLING only -- the computed value is bit-identical -- and it is
// deliberately narrow: a non-zero or non-constant amount keeps the infix
// form, a zero constant shared with a surviving consumer still renders
// there, and a zero-shift consumed by the compound-assign fold keeps
// today's `x <<= 0` rendering (that dead statement is out of scope).
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// `x << 0` renders the lhs alone; the dropped shift inlines into its
// consumer exactly as the bare name would.
// CHECK-LABEL: fn shl_zero(v0: i32) {
// CHECK-NEXT:    sink(v0);
// CHECK-NEXT:  }
emitrust.func @shl_zero(%arg0: i32) {
  %z = emitrust.constant <0 : i32> : i32
  %s = emitrust.shl %arg0, %z : i32
  emitrust.call_opaque "sink"(%s) : (i32) -> ()
  emitrust.return
}

// `x >> 0` folds too, in the train corpus's offset-0 bitfield READ shape:
// the folded text (an atom) sits bare as the `&` lhs -- `v0 & 255u16`,
// never `v0 >> 0u16 & 255u16`.
// CHECK-LABEL: fn shr_zero_mask(v0: u16) {
// CHECK-NEXT:    sink(v0 & 255u16);
// CHECK-NEXT:  }
emitrust.func @shr_zero_mask(%arg0: ui16) {
  %z = emitrust.constant <0 : ui16> : ui16
  %m = emitrust.constant <255 : ui16> : ui16
  %s = emitrust.shr %arg0, %z : ui16
  %a = emitrust.and %s, %m : ui16
  emitrust.call_opaque "sink"(%a) : (ui16) -> ()
  emitrust.return
}

// Inherited rank, tight side: a folded `(a & m) << 0` carries BitAnd rank,
// which binds tighter than the consuming `|` -- no parens (the offset-0
// bitfield WRITE shape).
// CHECK-LABEL: fn prec_tight_lhs(v0: i32, v1: i32) {
// CHECK-NEXT:    sink(v0 & 255i32 | v1);
// CHECK-NEXT:  }
emitrust.func @prec_tight_lhs(%arg0: i32, %arg1: i32) {
  %m = emitrust.constant <255 : i32> : i32
  %a = emitrust.and %arg0, %m : i32
  %z = emitrust.constant <0 : i32> : i32
  %s = emitrust.shl %a, %z : i32
  %o = emitrust.or %s, %arg1 : i32
  emitrust.call_opaque "sink"(%o) : (i32) -> ()
  emitrust.return
}

// Inherited rank, loose side -- the fold's risk center: a folded
// `(a | b) << 0` carries BitOr rank, LOOSER than the consuming `&`, so the
// parens must survive. A stale Shift-rank classification would emit
// `v0 | v1 & 255i32` and regroup the expression -- a miscompile.
// CHECK-LABEL: fn prec_loose_lhs(v0: i32, v1: i32) {
// CHECK-NEXT:    sink((v0 | v1) & 255i32);
// CHECK-NEXT:  }
emitrust.func @prec_loose_lhs(%arg0: i32, %arg1: i32) {
  %o = emitrust.or %arg0, %arg1 : i32
  %z = emitrust.constant <0 : i32> : i32
  %s = emitrust.shl %o, %z : i32
  %m = emitrust.constant <255 : i32> : i32
  %a = emitrust.and %s, %m : i32
  emitrust.call_opaque "sink"(%a) : (i32) -> ()
  emitrust.return
}

// Inherited trailing-cast flag: a folded `(x as i64) << 0` ends in a bare
// cast, so directly left of a real shift it must parenthesize (rustc
// parses `x as i64 << 2` as generic arguments on the type -- a hard
// error). Dropping the flag with the shift would emit the misparse.
// CHECK-LABEL: fn ends_in_cast(v0: i32) {
// CHECK-NEXT:    sink((v0 as i64) << 2i64);
// CHECK-NEXT:  }
emitrust.func @ends_in_cast(%arg0: i32) {
  %c = emitrust.cast %arg0 : i32 to i64
  %z = emitrust.constant <0 : i64> : i64
  %s = emitrust.shl %c, %z : i64
  %two = emitrust.constant <2 : i64> : i64
  %s2 = emitrust.shl %s, %two : i64
  emitrust.call_opaque "sink"(%s2) : (i64) -> ()
  emitrust.return
}

// Narrowness: a NON-zero constant amount and a NON-constant amount keep the
// infix rendering exactly.
// CHECK-LABEL: fn unfolded_amounts(v0: i32, v1: i32) {
// CHECK-NEXT:    sink2(v0 << 3i32, v0 >> v1);
// CHECK-NEXT:  }
emitrust.func @unfolded_amounts(%arg0: i32, %arg1: i32) {
  %c3 = emitrust.constant <3 : i32> : i32
  %s1 = emitrust.shl %arg0, %c3 : i32
  %s2 = emitrust.shr %arg0, %arg1 : i32
  emitrust.call_opaque "sink2"(%s1, %s2) : (i32, i32) -> ()
  emitrust.return
}

// A zero constant SHARED with a surviving consumer still renders there;
// only the never-rendered shift-amount use is dropped from read-tracking.
// CHECK-LABEL: fn shared_zero(v0: i32) {
// CHECK-NEXT:    sink2(v0, 0i32);
// CHECK-NEXT:  }
emitrust.func @shared_zero(%arg0: i32) {
  %z = emitrust.constant <0 : i32> : i32
  %s = emitrust.shl %arg0, %z : i32
  emitrust.call_opaque "sink2"(%s, %z) : (i32, i32) -> ()
  emitrust.return
}

// A MULTI-use folded shift keeps its own binding (only constants
// duplicate); the binding's right-hand side is the folded lhs. The dropped
// zero constant still consumes a name slot (v1), keeping the surviving
// v-numbering identical to the un-dropped rendering.
// CHECK-LABEL: fn multi_use(v0: i32) {
// CHECK-NEXT:    let v2: i32 = v0;
// CHECK-NEXT:    sink2(v2, v2);
// CHECK-NEXT:  }
emitrust.func @multi_use(%arg0: i32) {
  %z = emitrust.constant <0 : i32> : i32
  %s = emitrust.shl %arg0, %z : i32
  emitrust.call_opaque "sink2"(%s, %s) : (i32, i32) -> ()
  emitrust.return
}

// Compound-assign interplay: a self-referential `s = s << 0` still takes
// the FR-63 assign_op_pattern path and renders `s <<= 0i32;` exactly as
// today -- the amount stays read there (the carve-out in
// `isDroppedZeroShiftAmount`), so no binding is orphaned and no new
// dead-statement drop is introduced.
// CHECK-LABEL: fn compound_zero() {
// CHECK-NEXT:    let mut s: i32 = 5;
// CHECK-NEXT:    s <<= 0i32;
// CHECK-NEXT:    sink(s);
// CHECK-NEXT:  }
emitrust.func @compound_zero() {
  %s = emitrust.variable named "s" <5 : i32> : !emitrust.lvalue<i32>
  %l = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  %z = emitrust.constant <0 : i32> : i32
  %n = emitrust.shl %l, %z : i32
  emitrust.assign %s = %n : !emitrust.lvalue<i32>
  %r = emitrust.load %s : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "sink"(%r) : (i32) -> ()
  emitrust.return
}

// A NON-self-referential assign of a zero-shift takes the plain assign
// path and receives the folded value text.
// CHECK-LABEL: fn assign_nonself(v0: i32) {
// CHECK-NEXT:    let mut t: i32 = 1;
// CHECK-NEXT:    t = v0;
// CHECK-NEXT:    sink(t);
// CHECK-NEXT:  }
emitrust.func @assign_nonself(%arg0: i32) {
  %t = emitrust.variable named "t" <1 : i32> : !emitrust.lvalue<i32>
  %z = emitrust.constant <0 : i32> : i32
  %n = emitrust.shl %arg0, %z : i32
  emitrust.assign %t = %n : !emitrust.lvalue<i32>
  %r = emitrust.load %t : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "sink"(%r) : (i32) -> ()
  emitrust.return
}

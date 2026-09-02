// FR-186: the parenthesization policy (`needsParens` in
// lib/Target/Rust/TranslateToRust.cpp) wraps a binary operand not only
// where the Rust grammar REQUIRES it, but also where the mix of operator
// FAMILIES makes the binding non-obvious: an arithmetic operand (`+ - * /
// %`) directly under a bitwise-family parent (`| ^ & << >>`) is
// parenthesized even though it binds tighter and would parse identically
// bare.
//
// This is a FIDELITY invariant first and a lint second. The C author of
// test/EndToEnd/enum-unsigned-object-relational.c:55 wrote
// `seeds[(argc - 1) & 3]` and the emitter DROPPED those parentheses,
// re-spelling the source as `argc - 1i32 & 3i32`; restoring them is
// faithful to the source. `clippy::precedence` agrees, and the exact
// family rule pinned here was MEASURED against clippy 0.1.96, not
// assumed: an arithmetic operand under a bitwise or shift parent is
// flagged (both LHS and RHS); a bitwise operand under an arithmetic
// parent is not (it already carries grammar-required parens); a shift
// operand under a bitwise parent is not (shift is in the bitwise family);
// same-family nesting is not; and a `Unary`, `Cast`, or method-call
// operand is not.
//
// The change is pure SPELLING: parens never move stdout, and rustc does
// not consider these parens redundant (measured: `unused_parens`, DENIED
// in every emitted crate, stays silent on a parenthesized binary
// operand), so the `Stmt`/`Cond`/`Delimited` never-parenthesize positions
// are untouched -- pinned below along with the `endsInCast` shift /
// comparison generic-args fence, which is a correctness rule and must not
// be perturbed by the new style rule.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

//===----------------------------------------------------------------===//
// Arithmetic operand under a bitwise-family parent: NEW parens.
//===----------------------------------------------------------------===//

// The measured case, in miniature: `(argc - 1) & 3`.
// CHECK-LABEL: fn sub_lhs_of_bitand(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    (v0 - v1) & v2
// CHECK-NEXT:  }
emitrust.func @sub_lhs_of_bitand(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %s = emitrust.sub %arg0, %arg1 : i32
  %r = emitrust.and %s, %arg2 : i32
  emitrust.return %r : i32
}

// CHECK-LABEL: fn sub_rhs_of_bitand(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    v0 & (v1 - v2)
// CHECK-NEXT:  }
emitrust.func @sub_rhs_of_bitand(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %s = emitrust.sub %arg1, %arg2 : i32
  %r = emitrust.and %arg0, %s : i32
  emitrust.return %r : i32
}

// CHECK-LABEL: fn mul_lhs_of_bitor(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    (v0 * v1) | v2
// CHECK-NEXT:  }
emitrust.func @mul_lhs_of_bitor(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %m = emitrust.mul %arg0, %arg1 : i32
  %r = emitrust.or %m, %arg2 : i32
  emitrust.return %r : i32
}

// CHECK-LABEL: fn add_rhs_of_bitxor(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    v0 ^ (v1 + v2)
// CHECK-NEXT:  }
emitrust.func @add_rhs_of_bitxor(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %a = emitrust.add %arg1, %arg2 : i32
  %r = emitrust.xor %arg0, %a : i32
  emitrust.return %r : i32
}

// A shift parent behaves exactly like the other three: clippy flags
// `a + b << c` and `a << b + c` alike.
// CHECK-LABEL: fn add_lhs_of_shift(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    (v0 + v1) << v2
// CHECK-NEXT:  }
emitrust.func @add_lhs_of_shift(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %a = emitrust.add %arg0, %arg1 : i32
  %r = emitrust.shl %a, %arg2 : i32
  emitrust.return %r : i32
}

// CHECK-LABEL: fn add_rhs_of_shift(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    v0 >> (v1 + v2)
// CHECK-NEXT:  }
emitrust.func @add_rhs_of_shift(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %a = emitrust.add %arg1, %arg2 : i32
  %r = emitrust.shr %arg0, %a : i32
  emitrust.return %r : i32
}

// `%` is in the arithmetic family too (clippy flags `a % b ^ c`).
// CHECK-LABEL: fn rem_lhs_of_bitxor(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    (v0 % v1) ^ v2
// CHECK-NEXT:  }
emitrust.func @rem_lhs_of_bitxor(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %m = emitrust.rem %arg0, %arg1 : i32
  %r = emitrust.xor %m, %arg2 : i32
  emitrust.return %r : i32
}

//===----------------------------------------------------------------===//
// Same family: NO new parens (the existing rank rule alone decides).
//===----------------------------------------------------------------===//

// CHECK-LABEL: fn arith_same_family(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    v0 - v1 + v2
// CHECK-NEXT:  }
emitrust.func @arith_same_family(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %s = emitrust.sub %arg0, %arg1 : i32
  %r = emitrust.add %s, %arg2 : i32
  emitrust.return %r : i32
}

// A tighter arithmetic operand under an arithmetic parent stays bare.
// CHECK-LABEL: fn mul_lhs_of_add(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    v0 * v1 + v2
// CHECK-NEXT:  }
emitrust.func @mul_lhs_of_add(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %m = emitrust.mul %arg0, %arg1 : i32
  %r = emitrust.add %m, %arg2 : i32
  emitrust.return %r : i32
}

// Bitwise under bitwise follows the existing rank rule and nothing else.
// CHECK-LABEL: fn bitand_lhs_of_bitor(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    v0 & v1 | v2
// CHECK-NEXT:  }
emitrust.func @bitand_lhs_of_bitor(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %a = emitrust.and %arg0, %arg1 : i32
  %r = emitrust.or %a, %arg2 : i32
  emitrust.return %r : i32
}

// MEASURED: clippy does NOT flag `a << b & c` -- a shift operand is in
// the bitwise family, so it stays bare under a bitwise parent, LHS ...
// CHECK-LABEL: fn shift_lhs_of_bitand(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    v0 << v1 & v2
// CHECK-NEXT:  }
emitrust.func @shift_lhs_of_bitand(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %s = emitrust.shl %arg0, %arg1 : i32
  %r = emitrust.and %s, %arg2 : i32
  emitrust.return %r : i32
}

// ... and RHS (the rank rule wraps nothing here either: Shift binds
// tighter than BitAnd).
// CHECK-LABEL: fn shift_rhs_of_bitand(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    v0 & v1 << v2
// CHECK-NEXT:  }
emitrust.func @shift_rhs_of_bitand(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %s = emitrust.shl %arg1, %arg2 : i32
  %r = emitrust.and %arg0, %s : i32
  emitrust.return %r : i32
}

// MEASURED: clippy does NOT flag a `Cast` operand under a bitwise parent
// (`u as i64 & c` is left alone), and the family rule must not invent
// parens there -- only the endsInCast fence may, and a bitwise parent is
// not one of its two triggers.
// CHECK-LABEL: fn cast_lhs_of_bitand(v0: i16, v1: i32) -> i32 {
// CHECK-NEXT:    v0 as i32 & v1
// CHECK-NEXT:  }
emitrust.func @cast_lhs_of_bitand(%arg0: i16, %arg1: i32) -> i32 {
  %c = emitrust.cast %arg0 : i16 to i32
  %r = emitrust.and %c, %arg1 : i32
  emitrust.return %r : i32
}

//===----------------------------------------------------------------===//
// The reverse nesting: a bitwise operand under an arithmetic parent.
// MEASURED: clippy does not flag it, and it can never appear bare anyway
// -- every bitwise rank is LOOSER than every arithmetic one, so the
// pre-existing rank rule already wraps it. Pinned so the family rule is
// visibly not what produces these parens.
//===----------------------------------------------------------------===//

// CHECK-LABEL: fn bitand_lhs_of_add(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    (v0 & v1) + v2
// CHECK-NEXT:  }
emitrust.func @bitand_lhs_of_add(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %a = emitrust.and %arg0, %arg1 : i32
  %r = emitrust.add %a, %arg2 : i32
  emitrust.return %r : i32
}

// CHECK-LABEL: fn shift_rhs_of_mul(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    v0 * (v1 << v2)
// CHECK-NEXT:  }
emitrust.func @shift_rhs_of_mul(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %s = emitrust.shl %arg1, %arg2 : i32
  %r = emitrust.mul %arg0, %s : i32
  emitrust.return %r : i32
}

//===----------------------------------------------------------------===//
// The endsInCast generic-args fence is a CORRECTNESS rule and still
// fires, unperturbed by the new style rule.
//===----------------------------------------------------------------===//

// CHECK-LABEL: fn cast_lhs_of_shift(v0: i32, v1: i64) -> i64 {
// CHECK-NEXT:    (v0 as i64) << v1
// CHECK-NEXT:  }
emitrust.func @cast_lhs_of_shift(%arg0: i32, %arg1: i64) -> i64 {
  %c = emitrust.cast %arg0 : i32 to i64
  %r = emitrust.shl %c, %arg1 : i64
  emitrust.return %r : i64
}

// The fence follows the TRAILING token: an arithmetic operand that ends
// in a bare cast, under a shift, would need parens for BOTH reasons and
// gets exactly one pair.
// CHECK-LABEL: fn add_trailing_cast_lhs_of_shift(v0: i64, v1: i32, v2: i64) -> i64 {
// CHECK-NEXT:    (v0 + v1 as i64) << v2
// CHECK-NEXT:  }
emitrust.func @add_trailing_cast_lhs_of_shift(%arg0: i64, %arg1: i32, %arg2: i64) -> i64 {
  %c = emitrust.cast %arg1 : i32 to i64
  %a = emitrust.add %arg0, %c : i64
  %r = emitrust.shl %a, %arg2 : i64
  emitrust.return %r : i64
}

//===----------------------------------------------------------------===//
// The three never-parenthesize positions (`unused_parens` is DENIED in
// every emitted crate: a redundant paren there is a BUILD FAILURE, not a
// warning). The family rule fires in BinLhs/BinRhs only, so the outer
// expression stays bare in each.
//===----------------------------------------------------------------===//

// Stmt (tail expression): the whole `&` expression is bare; only its
// inner arithmetic operand gained parens.
// CHECK-LABEL: fn stmt_position_bare(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    (v0 - v1) & v2
// CHECK-NEXT:  }
emitrust.func @stmt_position_bare(%arg0: i32, %arg1: i32, %arg2: i32) -> i32 {
  %s = emitrust.sub %arg0, %arg1 : i32
  %r = emitrust.and %s, %arg2 : i32
  emitrust.return %r : i32
}

// Cond: the `if` scrutinee is bare, and the bitwise operand of the
// comparison is bare too (BitAnd binds tighter than Compare).
// CHECK-LABEL: fn cond_position_bare(v0: i32, v1: i32, v2: i32) {
// CHECK-NEXT:    if (v0 - v1) & v2 == 0i32 {
// CHECK-NEXT:        side_effect();
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @cond_position_bare(%arg0: i32, %arg1: i32, %arg2: i32) {
  %s = emitrust.sub %arg0, %arg1 : i32
  %a = emitrust.and %s, %arg2 : i32
  %z = emitrust.constant <0 : i32> : i32
  %p = emitrust.cmp eq, %a, %z : (i32, i32) -> i1
  emitrust.if %p {
    emitrust.call_opaque "side_effect"() : () -> ()
  }
  emitrust.return
}

// Delimited (a call argument): bare at the top, parenthesized inside.
// CHECK-LABEL: fn delimited_position_bare(v0: i32, v1: i32, v2: i32) {
// CHECK-NEXT:    sink((v0 - v1) & v2);
// CHECK-NEXT:  }
emitrust.func @delimited_position_bare(%arg0: i32, %arg1: i32, %arg2: i32) {
  %s = emitrust.sub %arg0, %arg1 : i32
  %a = emitrust.and %s, %arg2 : i32
  emitrust.call_opaque "sink"(%a) : (i32) -> ()
  emitrust.return
}

// FR / C99-20: convert-arith-to-emitrust lowers scalar constants, signed
// binary arithmetic, the bitwise and shift operations, the comparisons
// with an exact Rust operator (signed integer predicates;
// oeq/olt/ole/ogt/oge/une float predicates), scalar selects, and signed
// casts (index_castui with zero-extension-exact rendering).
// RUN: emitrust-opt --convert-arith-to-emitrust %s | FileCheck %s

// CHECK-LABEL: func.func @constants
// CHECK:         emitrust.constant <42 : i32> : i32
// CHECK:         emitrust.constant <7 : index> : index
// CHECK:         emitrust.constant <2.500000e+00 : f32> : f32
// CHECK:         emitrust.constant <true> : i1
// CHECK-NOT:     arith.constant
func.func @constants() -> (i32, index, f32, i1) {
  %0 = arith.constant 42 : i32
  %1 = arith.constant 7 : index
  %2 = arith.constant 2.5 : f32
  %3 = arith.constant true
  return %0, %1, %2, %3 : i32, index, f32, i1
}

// The operands are deliberately independent: chained forms such as
// subi(addi(a, b), b) are folded away by the conversion driver before the
// patterns run, which is legal but would hide the sub pattern.
// CHECK-LABEL: func.func @int_binops
// CHECK:         emitrust.add %arg0, %arg1 : i32
// CHECK:         emitrust.sub %arg0, %arg1 : i32
// CHECK:         emitrust.mul %arg0, %arg1 : i32
// CHECK:         emitrust.div %arg0, %arg1 : i32
// CHECK:         emitrust.rem %arg0, %arg1 : i32
// CHECK-NOT:     arith.
func.func @int_binops(%a: i32, %b: i32) -> (i32, i32, i32, i32, i32) {
  %0 = arith.addi %a, %b : i32
  %1 = arith.subi %a, %b : i32
  %2 = arith.muli %a, %b : i32
  %3 = arith.divsi %a, %b : i32
  %4 = arith.remsi %a, %b : i32
  return %0, %1, %2, %3, %4 : i32, i32, i32, i32, i32
}

// Bitwise and/or/xor and the left shift are sign-agnostic and convert on
// any integer type; shrsi converts on signless/signed types only (Rust's
// `>>` on iN is the arithmetic shift). The unsigned-semantics shrui,
// divui, remui, and cmpi ult/ule/ugt/uge only convert on unsigned
// IntegerType operands — which the Arith verifier itself does not admit
// today, so on the signless types the importer produces they stay illegal
// (covered by unsigned-invalid.mlir) rather than silently converting to
// the signed Rust operators.
// CHECK-LABEL: func.func @bit_binops
// CHECK:         emitrust.and %arg0, %arg1 : i32
// CHECK:         emitrust.or %arg0, %arg1 : i32
// CHECK:         emitrust.xor %arg0, %arg1 : i32
// CHECK:         emitrust.shl %arg0, %arg1 : i32
// CHECK:         emitrust.shr %arg0, %arg1 : i32
// CHECK-NOT:     arith.
func.func @bit_binops(%a: i32, %b: i32) -> (i32, i32, i32, i32, i32) {
  %0 = arith.andi %a, %b : i32
  %1 = arith.ori %a, %b : i32
  %2 = arith.xori %a, %b : i32
  %3 = arith.shli %a, %b : i32
  %4 = arith.shrsi %a, %b : i32
  return %0, %1, %2, %3, %4 : i32, i32, i32, i32, i32
}

// The bitwise operations also convert on the narrower and wider widths.
// The operands are independent so that no arith folder (e.g. x & x = x)
// removes the operation before the patterns run.
// CHECK-LABEL: func.func @bit_binops_widths
// CHECK:         emitrust.and %arg0, %arg1 : i8
// CHECK:         emitrust.shr %arg2, %arg3 : i64
// CHECK-NOT:     arith.
func.func @bit_binops_widths(%a: i8, %b: i8, %c: i64, %d: i64) -> (i8, i64) {
  %0 = arith.andi %a, %b : i8
  %1 = arith.shrsi %c, %d : i64
  return %0, %1 : i8, i64
}

// CHECK-LABEL: func.func @float_binops
// CHECK:         emitrust.add %arg0, %arg1 : f64
// CHECK:         emitrust.sub %arg0, %arg1 : f64
// CHECK:         emitrust.mul %arg0, %arg1 : f64
// CHECK:         emitrust.div %arg0, %arg1 : f64
// CHECK-NOT:     arith.
func.func @float_binops(%a: f64, %b: f64) -> (f64, f64, f64, f64) {
  %0 = arith.addf %a, %b : f64
  %1 = arith.subf %a, %b : f64
  %2 = arith.mulf %a, %b : f64
  %3 = arith.divf %a, %b : f64
  return %0, %1, %2, %3 : f64, f64, f64, f64
}

// C unary minus on a float operand imports as arith.negf; EmitRust has no
// unary negation op, so it lowers as `0.0 - x` — an emitrust.sub against a
// zero constant of the operand type — mirroring the integer negation
// spelling `0 - x` the importer uses for signless integers.
// CHECK-LABEL: func.func @float_neg
// CHECK:         %[[Z64:.*]] = emitrust.constant <0.000000e+00 : f64> : f64
// CHECK:         emitrust.sub %[[Z64]], %arg0 : f64
// CHECK:         %[[Z32:.*]] = emitrust.constant <0.000000e+00 : f32> : f32
// CHECK:         emitrust.sub %[[Z32]], %arg1 : f32
// CHECK-NOT:     arith.negf
func.func @float_neg(%a: f64, %b: f32) -> (f64, f32) {
  %0 = arith.negf %a : f64
  %1 = arith.negf %b : f32
  return %0, %1 : f64, f32
}

// CHECK-LABEL: func.func @int_cmps
// CHECK:         emitrust.cmp eq, %arg0, %arg1 : (i32, i32) -> i1
// CHECK:         emitrust.cmp ne, %arg0, %arg1 : (i32, i32) -> i1
// CHECK:         emitrust.cmp lt, %arg0, %arg1 : (i32, i32) -> i1
// CHECK:         emitrust.cmp le, %arg0, %arg1 : (i32, i32) -> i1
// CHECK:         emitrust.cmp gt, %arg0, %arg1 : (i32, i32) -> i1
// CHECK:         emitrust.cmp ge, %arg0, %arg1 : (i32, i32) -> i1
// CHECK-NOT:     arith.cmpi
func.func @int_cmps(%a: i32, %b: i32) -> (i1, i1, i1, i1, i1, i1) {
  %0 = arith.cmpi eq, %a, %b : i32
  %1 = arith.cmpi ne, %a, %b : i32
  %2 = arith.cmpi slt, %a, %b : i32
  %3 = arith.cmpi sle, %a, %b : i32
  %4 = arith.cmpi sgt, %a, %b : i32
  %5 = arith.cmpi sge, %a, %b : i32
  return %0, %1, %2, %3, %4, %5 : i1, i1, i1, i1, i1, i1
}

// Rust float `==`/`<`/`<=`/`>`/`>=` are the IEEE ordered comparisons and
// `!=` is unordered-or-unequal, so oeq/olt/ole/ogt/oge and une map exactly.
// `one` and the remaining unordered predicates have no Rust operator and
// stay illegal, covered by unsigned-invalid.mlir.
// CHECK-LABEL: func.func @float_cmps
// CHECK:         emitrust.cmp eq, %arg0, %arg1 : (f32, f32) -> i1
// CHECK:         emitrust.cmp ne, %arg0, %arg1 : (f32, f32) -> i1
// CHECK:         emitrust.cmp lt, %arg0, %arg1 : (f32, f32) -> i1
// CHECK:         emitrust.cmp le, %arg0, %arg1 : (f32, f32) -> i1
// CHECK:         emitrust.cmp gt, %arg0, %arg1 : (f32, f32) -> i1
// CHECK:         emitrust.cmp ge, %arg0, %arg1 : (f32, f32) -> i1
// CHECK-NOT:     arith.cmpf
func.func @float_cmps(%a: f32, %b: f32) -> (i1, i1, i1, i1, i1, i1) {
  %0 = arith.cmpf oeq, %a, %b : f32
  %1 = arith.cmpf une, %a, %b : f32
  %2 = arith.cmpf olt, %a, %b : f32
  %3 = arith.cmpf ole, %a, %b : f32
  %4 = arith.cmpf ogt, %a, %b : f32
  %5 = arith.cmpf oge, %a, %b : f32
  return %0, %1, %2, %3, %4, %5 : i1, i1, i1, i1, i1, i1
}

// CHECK-LABEL: func.func @casts
// CHECK:         emitrust.cast %arg0 : i16 to i32
// CHECK:         emitrust.cast %arg1 : i64 to i32
// CHECK:         emitrust.cast %arg2 : i32 to f64
// CHECK:         emitrust.cast %arg3 : f64 to i32
// CHECK:         emitrust.cast %arg4 : index to i64
// CHECK:         emitrust.cast %arg5 : i1 to i32
// CHECK:         emitrust.cast %arg6 : f32 to f64
// CHECK:         emitrust.cast %arg3 : f64 to f32
// CHECK-NOT:     arith.
func.func @casts(%a: i16, %b: i64, %c: i32, %d: f64, %e: index, %f: i1,
                 %g: f32)
    -> (i32, i32, f64, i32, i64, i32, f64, f32) {
  %0 = arith.extsi %a : i16 to i32
  %1 = arith.trunci %b : i64 to i32
  %2 = arith.sitofp %c : i32 to f64
  %3 = arith.fptosi %d : f64 to i32
  %4 = arith.index_cast %e : index to i64
  // extui is legal only from i1 (Rust bool-as-int); wider sources stay
  // illegal, covered by unsigned-invalid.mlir.
  %5 = arith.extui %f : i1 to i32
  // extf is exact in Rust `as`; truncf rounds to nearest (ties to even),
  // matching C's double-to-float conversion. Both feed the C float
  // promotions around printf %f (f32 arguments) and float locals.
  %6 = arith.extf %g : f32 to f64
  %7 = arith.truncf %d : f64 to f32
  return %0, %1, %2, %3, %4, %5, %6, %7
      : i32, i32, f64, i32, i64, i32, f64, f32
}

// index_castui zero-extends. A single Rust `as usize` would SIGN-extend a
// signed iN source (lift-cf-to-scf feeds user switch scrutinees through
// this cast, so negative values do occur), so the lowering hops through
// the unsigned type of the same width: `v as uN as usize`. An i1 source
// keeps the single cast (bool as usize is 0 or 1), as does an index
// source (usize as iN truncation is bit-exact regardless of extension).
// CHECK-LABEL: func.func @index_castui_zext
// CHECK:         %[[U32:.*]] = emitrust.cast %arg0 : i32 to ui32
// CHECK:         emitrust.cast %[[U32]] : ui32 to index
// CHECK:         %[[U64:.*]] = emitrust.cast %arg1 : i64 to ui64
// CHECK:         emitrust.cast %[[U64]] : ui64 to index
// CHECK:         emitrust.cast %arg2 : i1 to index
// CHECK:         emitrust.cast %arg3 : index to i32
// CHECK-NOT:     arith.
func.func @index_castui_zext(%a: i32, %b: i64, %c: i1, %d: index)
    -> (index, index, index, i32) {
  %0 = arith.index_castui %a : i32 to index
  %1 = arith.index_castui %b : i64 to index
  %2 = arith.index_castui %c : i1 to index
  %3 = arith.index_castui %d : index to i32
  return %0, %1, %2, %3 : index, index, index, i32
}

// arith.select lowers to emitrust.select, rendered as a Rust
// `if cond { a } else { b }` expression; the canonicalizer synthesizes it
// from folded conditional chains.
// CHECK-LABEL: func.func @selects
// CHECK:         emitrust.select %arg0, %arg1, %arg2 : i32
// CHECK:         emitrust.select %arg0, %arg3, %arg4 : f64
// CHECK-NOT:     arith.
func.func @selects(%c: i1, %a: i32, %b: i32, %x: f64, %y: f64)
    -> (i32, f64) {
  %0 = arith.select %c, %a, %b : i32
  %1 = arith.select %c, %x, %y : f64
  return %0, %1 : i32, f64
}

// A truncation to i1 selects the low bit, which has no Rust `as`
// equivalent; it lowers to a remainder plus a comparison against zero.
// CHECK-LABEL: func.func @trunc_to_bool
// CHECK:         %[[TWO:.*]] = emitrust.constant <2 : i32> : i32
// CHECK:         %[[REM:.*]] = emitrust.rem %arg0, %[[TWO]] : i32
// CHECK:         %[[ZERO:.*]] = emitrust.constant <0 : i32> : i32
// CHECK:         emitrust.cmp ne, %[[REM]], %[[ZERO]] : (i32, i32) -> i1
// CHECK-NOT:     arith.
func.func @trunc_to_bool(%a: i32) -> i1 {
  %0 = arith.trunci %a : i32 to i1
  return %0 : i1
}

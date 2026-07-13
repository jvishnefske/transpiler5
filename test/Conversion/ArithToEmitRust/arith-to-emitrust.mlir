// FR: convert-arith-to-emitrust lowers scalar constants, signed binary
// arithmetic, signed/ordered comparisons, and signed casts.
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
  %1 = arith.cmpf one, %a, %b : f32
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
// CHECK-NOT:     arith.
func.func @casts(%a: i16, %b: i64, %c: i32, %d: f64, %e: index, %f: i1)
    -> (i32, i32, f64, i32, i64, i32) {
  %0 = arith.extsi %a : i16 to i32
  %1 = arith.trunci %b : i64 to i32
  %2 = arith.sitofp %c : i32 to f64
  %3 = arith.fptosi %d : f64 to i32
  %4 = arith.index_cast %e : index to i64
  // extui is legal only from i1 (Rust bool-as-int); wider sources stay
  // illegal, covered by unsigned-invalid.mlir.
  %5 = arith.extui %f : i1 to i32
  return %0, %1, %2, %3, %4, %5 : i32, i32, f64, i32, i64, i32
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

// FR: the composite convert-to-emitrust pass feeds the Rust emitter: a
// scalar function goes from core dialects to plausible Rust source.
// RUN: emitrust-opt --convert-to-emitrust %s | emitrust-translate --mlir-to-rust | FileCheck %s

// FR-61d: the single-use product inlines into the tail expression.
// CHECK-LABEL: fn madd(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    v0 * v1 + v2
// CHECK-NEXT:  }
func.func @madd(%a: i32, %b: i32, %c: i32) -> i32 {
  %0 = arith.muli %a, %b : i32
  %1 = arith.addi %0, %c : i32
  return %1 : i32
}

// FR-61d: the single-use comparison inlines into the FR-61b if-expression
// binding's condition, the unused SSA-destruction default constant drops,
// and (slice 3) the binding folds into the tail if-expression.
// CHECK-LABEL: fn max(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    if v0 > v1 {
// CHECK-NEXT:      v0
// CHECK-NEXT:    } else {
// CHECK-NEXT:      v1
// CHECK-NEXT:    }
// CHECK-NEXT:  }
func.func @max(%a: i32, %b: i32) -> i32 {
  %cond = arith.cmpi sgt, %a, %b : i32
  %0 = scf.if %cond -> (i32) {
    scf.yield %a : i32
  } else {
    scf.yield %b : i32
  }
  return %0 : i32
}

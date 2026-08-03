// FR: the composite convert-to-emitrust pass feeds the Rust emitter: a
// scalar function goes from core dialects to plausible Rust source.
// RUN: emitrust-opt --convert-to-emitrust %s | emitrust-translate --mlir-to-rust | FileCheck %s

// CHECK-LABEL: fn madd(v0: i32, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    let v3: i32 = v0 * v1;
// CHECK-NEXT:    v3 + v2
// CHECK-NEXT:  }
func.func @madd(%a: i32, %b: i32, %c: i32) -> i32 {
  %0 = arith.muli %a, %b : i32
  %1 = arith.addi %0, %c : i32
  return %1 : i32
}

// CHECK-LABEL: fn max(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    let v2: bool = v0 > v1;
// CHECK-NEXT:    let _v3: i32 = 0;
// CHECK-NEXT:    let v4: i32 = if v2 {
// CHECK-NEXT:      v0
// CHECK-NEXT:    } else {
// CHECK-NEXT:      v1
// CHECK-NEXT:    };
// CHECK-NEXT:    v4
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

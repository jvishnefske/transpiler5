// RUN: emitrust-import-c %s | FileCheck %s

// A non-void function whose control falls off the end. C11 6.9.1p12 makes
// this defined as long as the caller never uses the missing value; the
// importer synthesizes a `return 0` (of the function's return type) at
// function finalization so the lowered function always yields a value.
// (Rust has no fall-off-the-end for value-returning functions, and the
// synthesized zero is only observable on executions that were undefined
// reads in C anyway.)

int falls_off(int x) {
  x = x + 1;
}

// The body runs, then the synthesized zero is returned.
// CHECK-LABEL: func.func @falls_off
// CHECK: arith.addi
// CHECK: %[[Z:.*]] = arith.constant 0 : i32
// CHECK: return %[[Z]] : i32

int one_arm(int x) {
  if (x > 0)
    return 7;
}

// One arm returns explicitly; the fall-through arm gets the synthesized
// `return 0` on its own path.
// CHECK-LABEL: func.func @one_arm
// CHECK: cf.cond_br
// CHECK: %[[SEVEN:.*]] = arith.constant 7 : i32
// CHECK: return %[[SEVEN]] : i32
// CHECK: %[[Z2:.*]] = arith.constant 0 : i32
// CHECK: return %[[Z2]] : i32

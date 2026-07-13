// FR: convert-scf-to-emitrust lowers scf.if with and without results;
// results become default-initialized mutable lets assigned in the regions.
// RUN: emitrust-opt --convert-scf-to-emitrust %s | FileCheck %s

// CHECK-LABEL: func.func @if_no_result
// CHECK:         emitrust.if %arg0 {
// CHECK-NEXT:      emitrust.call_opaque "then_branch"() : () -> ()
// CHECK-NEXT:    }
// CHECK-NOT:     scf.if
func.func @if_no_result(%cond: i1) {
  scf.if %cond {
    emitrust.call_opaque "then_branch"() : () -> ()
  }
  return
}

// CHECK-LABEL: func.func @if_else_no_result
// CHECK:         emitrust.if %arg0 {
// CHECK-NEXT:      emitrust.call_opaque "then_branch"() : () -> ()
// CHECK-NEXT:    } else {
// CHECK-NEXT:      emitrust.call_opaque "else_branch"() : () -> ()
// CHECK-NEXT:    }
func.func @if_else_no_result(%cond: i1) {
  scf.if %cond {
    emitrust.call_opaque "then_branch"() : () -> ()
  } else {
    emitrust.call_opaque "else_branch"() : () -> ()
  }
  return
}

// CHECK-LABEL: func.func @if_result
// CHECK:         %[[DEF:.*]] = emitrust.constant <0 : i32> : i32
// CHECK:         %[[RES:.*]] = emitrust.let mut %[[DEF]] : i32
// CHECK:         emitrust.if %arg0 {
// CHECK-NEXT:      emitrust.assign %[[RES]] = %arg1 : i32
// CHECK-NEXT:    } else {
// CHECK-NEXT:      emitrust.assign %[[RES]] = %arg2 : i32
// CHECK-NEXT:    }
// CHECK:         return %[[RES]] : i32
func.func @if_result(%cond: i1, %a: i32, %b: i32) -> i32 {
  %0 = scf.if %cond -> (i32) {
    scf.yield %a : i32
  } else {
    scf.yield %b : i32
  }
  return %0 : i32
}

// CHECK-LABEL: func.func @if_two_results
// CHECK:         %[[DEF0:.*]] = emitrust.constant <0 : i32> : i32
// CHECK:         %[[RES0:.*]] = emitrust.let mut %[[DEF0]] : i32
// CHECK:         %[[DEF1:.*]] = emitrust.constant <0.000000e+00 : f64> : f64
// CHECK:         %[[RES1:.*]] = emitrust.let mut %[[DEF1]] : f64
// CHECK:         emitrust.if %arg0 {
// CHECK-NEXT:      emitrust.assign %[[RES0]] = %arg1 : i32
// CHECK-NEXT:      emitrust.assign %[[RES1]] = %arg2 : f64
// CHECK-NEXT:    } else {
// CHECK-NEXT:      emitrust.assign %[[RES0]] = %arg1 : i32
// CHECK-NEXT:      emitrust.assign %[[RES1]] = %arg2 : f64
// CHECK-NEXT:    }
// CHECK:         return %[[RES0]], %[[RES1]] : i32, f64
func.func @if_two_results(%cond: i1, %a: i32, %b: f64) -> (i32, f64) {
  %0:2 = scf.if %cond -> (i32, f64) {
    scf.yield %a, %b : i32, f64
  } else {
    scf.yield %a, %b : i32, f64
  }
  return %0#0, %0#1 : i32, f64
}

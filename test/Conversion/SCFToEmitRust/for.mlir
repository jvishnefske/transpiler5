// FR: convert-scf-to-emitrust lowers scf.for to emitrust.for; iteration
// arguments become mutable lets assigned at the end of the body.
// RUN: emitrust-opt --convert-scf-to-emitrust %s | FileCheck %s

// CHECK-LABEL: func.func @plain_loop
// CHECK:         emitrust.for %[[IV:.*]] = %arg0 to %arg1 step %arg2 {
// CHECK-NEXT:      emitrust.call_opaque "body"(%[[IV]]) : (index) -> ()
// CHECK-NEXT:    }
// CHECK-NOT:     scf.for
func.func @plain_loop(%lb: index, %ub: index, %step: index) {
  scf.for %i = %lb to %ub step %step {
    emitrust.call_opaque "body"(%i) : (index) -> ()
  }
  return
}

// CHECK-LABEL: func.func @iter_args_loop
// CHECK:         %[[ACC:.*]] = emitrust.let mut %arg3 : i32
// CHECK:         emitrust.for %{{.*}} = %arg0 to %arg1 step %arg2 {
// CHECK:           %[[ONE:.*]] = emitrust.constant <1 : i32> : i32
// CHECK:           %[[NEXT:.*]] = emitrust.add %[[ACC]], %[[ONE]] : i32
// CHECK:           emitrust.assign %[[ACC]] = %[[NEXT]] : i32
// CHECK:         }
// CHECK:         return %[[ACC]] : i32
func.func @iter_args_loop(%lb: index, %ub: index, %step: index,
                          %init: i32) -> i32 {
  %0 = scf.for %i = %lb to %ub step %step iter_args(%acc = %init) -> (i32) {
    %one = emitrust.constant <1 : i32> : i32
    %next = emitrust.add %acc, %one : i32
    scf.yield %next : i32
  }
  return %0 : i32
}

// A swapping iter_args back-edge is a copy cycle: the yield sources are
// staged into immutable temporaries before any iteration let is assigned
// (the lost-copy problem of sequential rebinding).
// CHECK-LABEL: func.func @swap_iter_args
// CHECK:         %[[A:.*]] = emitrust.let mut %arg3 : i32
// CHECK:         %[[B:.*]] = emitrust.let mut %arg4 : i32
// CHECK:         emitrust.for %{{.*}} = %arg0 to %arg1 step %arg2 {
// CHECK-NEXT:      %[[TA:.*]] = emitrust.let %[[B]] : i32
// CHECK-NEXT:      %[[TB:.*]] = emitrust.let %[[A]] : i32
// CHECK-NEXT:      emitrust.assign %[[A]] = %[[TA]] : i32
// CHECK-NEXT:      emitrust.assign %[[B]] = %[[TB]] : i32
// CHECK:         }
// CHECK:         return %[[A]], %[[B]] : i32, i32
func.func @swap_iter_args(%lb: index, %ub: index, %step: index,
                          %ia: i32, %ib: i32) -> (i32, i32) {
  %0:2 = scf.for %i = %lb to %ub step %step
      iter_args(%a = %ia, %b = %ib) -> (i32, i32) {
    scf.yield %b, %a : i32, i32
  }
  return %0#0, %0#1 : i32, i32
}

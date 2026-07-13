// FR: convert-scf-to-emitrust lowers scf.index_switch (produced by
// --lift-cf-to-scf for multi-exit loops) to a nested emitrust.if/else
// chain over the discriminator, with results as mutable lets.
// RUN: emitrust-opt --convert-scf-to-emitrust %s | FileCheck %s

// CHECK-LABEL: func.func @two_cases
// CHECK:         %[[DEF:.*]] = emitrust.constant <0 : i32> : i32
// CHECK:         %[[RES:.*]] = emitrust.let mut %[[DEF]] : i32
// CHECK:         %[[C2:.*]] = emitrust.constant <2 : index> : index
// CHECK:         %[[IS2:.*]] = emitrust.cmp eq, %arg0, %[[C2]] : (index, index) -> i1
// CHECK:         emitrust.if %[[IS2]] {
// CHECK-NEXT:      emitrust.assign %[[RES]] = %arg1 : i32
// CHECK-NEXT:    } else {
// CHECK-NEXT:      %[[C5:.*]] = emitrust.constant <5 : index> : index
// CHECK-NEXT:      %[[IS5:.*]] = emitrust.cmp eq, %arg0, %[[C5]] : (index, index) -> i1
// CHECK-NEXT:      emitrust.if %[[IS5]] {
// CHECK-NEXT:        emitrust.assign %[[RES]] = %arg2 : i32
// CHECK-NEXT:      } else {
// CHECK-NEXT:        emitrust.assign %[[RES]] = %arg3 : i32
// CHECK-NEXT:      }
// CHECK-NEXT:    }
// CHECK:         return %[[RES]] : i32
// CHECK-NOT:     scf.index_switch
func.func @two_cases(%idx: index, %a: i32, %b: i32, %c: i32) -> i32 {
  %0 = scf.index_switch %idx -> i32
  case 2 {
    scf.yield %a : i32
  }
  case 5 {
    scf.yield %b : i32
  }
  default {
    scf.yield %c : i32
  }
  return %0 : i32
}

// The single-case shape --lift-cf-to-scf emits for loop break/continue
// discriminators.
// CHECK-LABEL: func.func @single_case
// CHECK:         %[[DEF:.*]] = emitrust.constant <0 : i32> : i32
// CHECK:         %[[RES:.*]] = emitrust.let mut %[[DEF]] : i32
// CHECK:         %[[C0:.*]] = emitrust.constant <0 : index> : index
// CHECK:         %[[IS0:.*]] = emitrust.cmp eq, %arg0, %[[C0]] : (index, index) -> i1
// CHECK:         emitrust.if %[[IS0]] {
// CHECK-NEXT:      emitrust.assign %[[RES]] = %arg1 : i32
// CHECK-NEXT:    } else {
// CHECK-NEXT:      emitrust.assign %[[RES]] = %arg2 : i32
// CHECK-NEXT:    }
// CHECK:         return %[[RES]] : i32
func.func @single_case(%idx: index, %a: i32, %b: i32) -> i32 {
  %0 = scf.index_switch %idx -> i32
  case 0 {
    scf.yield %a : i32
  }
  default {
    scf.yield %b : i32
  }
  return %0 : i32
}

// A result-less switch used purely for side effects.
// CHECK-LABEL: func.func @no_results
// CHECK:         %[[C1:.*]] = emitrust.constant <1 : index> : index
// CHECK:         %[[IS1:.*]] = emitrust.cmp eq, %arg0, %[[C1]] : (index, index) -> i1
// CHECK:         emitrust.if %[[IS1]] {
// CHECK-NEXT:      emitrust.call_opaque "case_one"() : () -> ()
// CHECK-NEXT:    } else {
// CHECK-NEXT:      emitrust.call_opaque "fallback"() : () -> ()
// CHECK-NEXT:    }
func.func @no_results(%idx: index) {
  scf.index_switch %idx
  case 1 {
    emitrust.call_opaque "case_one"() : () -> ()
    scf.yield
  }
  default {
    emitrust.call_opaque "fallback"() : () -> ()
    scf.yield
  }
  return
}

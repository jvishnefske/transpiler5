// FR: convert-scf-to-emitrust lowers scf.index_switch (produced by
// --lift-cf-to-scf for multi-exit loops) to emitrust.switch over the same
// discriminator and case values, with results as mutable lets assigned in
// the inlined case and default regions.
// RUN: emitrust-opt --convert-scf-to-emitrust %s | FileCheck %s

// CHECK-LABEL: func.func @two_cases
// CHECK:         %[[DEF:.*]] = emitrust.constant <0 : i32> : i32
// CHECK:         %[[RES:.*]] = emitrust.let mut %[[DEF]] : i32
// CHECK:         emitrust.switch %arg0 : index
// CHECK-NEXT:    case 2 {
// CHECK-NEXT:      emitrust.assign %[[RES]] = %arg1 : i32
// CHECK-NEXT:    }
// CHECK-NEXT:    case 5 {
// CHECK-NEXT:      emitrust.assign %[[RES]] = %arg2 : i32
// CHECK-NEXT:    }
// CHECK-NEXT:    default {
// CHECK-NEXT:      emitrust.assign %[[RES]] = %arg3 : i32
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
// CHECK:         emitrust.switch %arg0 : index
// CHECK-NEXT:    case 0 {
// CHECK-NEXT:      emitrust.assign %[[RES]] = %arg1 : i32
// CHECK-NEXT:    }
// CHECK-NEXT:    default {
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
// CHECK:         emitrust.switch %arg0 : index
// CHECK-NEXT:    case 1 {
// CHECK-NEXT:      emitrust.call_opaque "case_one"() : () -> ()
// CHECK-NEXT:    }
// CHECK-NEXT:    default {
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

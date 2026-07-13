// FR: convert-scf-to-emitrust lowers scf.while to emitrust.loop with
// mutable lets for the carried values and results, and an exit through
// emitrust.if + emitrust.break on the negated condition.
// RUN: emitrust-opt --convert-scf-to-emitrust %s | FileCheck %s

// CHECK-LABEL: func.func @count_up
// CHECK:         %[[CARRIED:.*]] = emitrust.let mut %arg0 : i32
// CHECK:         %[[DEF:.*]] = emitrust.constant <0 : i32> : i32
// CHECK:         %[[OUT:.*]] = emitrust.let mut %[[DEF]] : i32
// CHECK:         emitrust.loop {
// CHECK:           %[[COND:.*]] = emitrust.cmp lt, %[[CARRIED]], %arg1 : (i32, i32) -> i1
// CHECK:           %[[FALSE:.*]] = emitrust.constant <false> : i1
// CHECK:           %[[NEG:.*]] = emitrust.cmp eq, %[[COND]], %[[FALSE]] : (i1, i1) -> i1
// CHECK:           emitrust.if %[[NEG]] {
// CHECK-NEXT:        emitrust.assign %[[OUT]] = %[[CARRIED]] : i32
// CHECK-NEXT:        emitrust.break
// CHECK-NEXT:      }
// CHECK:           %[[ONE:.*]] = emitrust.constant <1 : i32> : i32
// CHECK:           %[[NEXT:.*]] = emitrust.add %[[CARRIED]], %[[ONE]] : i32
// CHECK:           emitrust.assign %[[CARRIED]] = %[[NEXT]] : i32
// CHECK:         }
// CHECK:         return %[[OUT]] : i32
// CHECK-NOT:     scf.while
func.func @count_up(%init: i32, %limit: i32) -> i32 {
  %0 = scf.while (%v = %init) : (i32) -> i32 {
    %cond = emitrust.cmp lt, %v, %limit : (i32, i32) -> i1
    scf.condition(%cond) %v : i32
  } do {
  ^bb0(%w: i32):
    %one = emitrust.constant <1 : i32> : i32
    %next = emitrust.add %w, %one : i32
    scf.yield %next : i32
  }
  return %0 : i32
}

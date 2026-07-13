// FR: convert-ub-to-emitrust lowers ub.poison to default-valued constants.
// RUN: emitrust-opt --convert-ub-to-emitrust %s | FileCheck %s

// CHECK-LABEL: func.func @poisons
// CHECK:         %[[I:.*]] = emitrust.constant <0 : i32> : i32
// CHECK:         %[[F:.*]] = emitrust.constant <0.000000e+00 : f32> : f32
// CHECK:         %[[B:.*]] = emitrust.constant <false> : i1
// CHECK:         %[[D:.*]] = emitrust.constant <0.000000e+00 : f64> : f64
// CHECK:         %[[X:.*]] = emitrust.constant <0 : index> : index
// CHECK:         return %[[I]], %[[F]], %[[B]], %[[D]], %[[X]]
// CHECK-NOT:     ub.poison
func.func @poisons() -> (i32, f32, i1, f64, index) {
  %0 = ub.poison : i32
  %1 = ub.poison : f32
  %2 = ub.poison : i1
  %3 = ub.poison : f64
  %4 = ub.poison : index
  return %0, %1, %2, %3, %4 : i32, f32, i1, f64, index
}

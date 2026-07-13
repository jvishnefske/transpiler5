// FR: convert-func-to-emitrust lowers func.func/return/call to
// emitrust.func/return/call_opaque.
// RUN: emitrust-opt --convert-func-to-emitrust %s | FileCheck %s

// CHECK-LABEL: emitrust.func @identity
// CHECK-SAME:    (%arg0: i32) -> i32
// CHECK:         emitrust.return %arg0 : i32
func.func @identity(%v: i32) -> i32 {
  return %v : i32
}

// CHECK-LABEL: emitrust.func @caller
// CHECK:         %[[R:.*]] = emitrust.call_opaque "identity"(%arg0) : (i32) -> i32
// CHECK:         emitrust.return %[[R]] : i32
func.func @caller(%v: i32) -> i32 {
  %0 = call @identity(%v) : (i32) -> i32
  return %0 : i32
}

// CHECK-LABEL: emitrust.func @no_result
// CHECK:         emitrust.call_opaque "identity"(%arg0) : (i32) -> i32
// CHECK:         emitrust.return{{$}}
func.func @no_result(%v: i32) {
  %0 = call @identity(%v) : (i32) -> i32
  return
}

// CHECK: emitrust.func private @decl(i32) -> i32
func.func private @decl(i32) -> i32

// CHECK-NOT: func.func
// CHECK-NOT: func.return
// CHECK-NOT: func.call

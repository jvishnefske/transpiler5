// RUN: emitrust-import-c %s | FileCheck %s

// A decayed array-of-function-pointers parameter (the 00209 `fptr4`
// shape): the pointee is an ordinary Copy `!emitrust.fn_ptr` value, so
// the parameter classifies as a slice like any subscripted pointer
// parameter, the element loads as a plain fn_ptr, and the call is an
// ordinary `emitrust.call_indirect`. A pointee that is itself a *data*
// pointer stays rejected (CTS-P5, pointers-local-invalid.c).

// CHECK-LABEL: func.func @apply
// CHECK-SAME: !emitrust.mut_ref<!emitrust.slice<!emitrust.fn_ptr<(i32) -> i32>>>
// CHECK: emitrust.subscript
// CHECK: emitrust.load {{.*}} -> !emitrust.fn_ptr<(i32) -> i32>
// CHECK: emitrust.call_indirect
int apply(int (*fns[2])(int), int which, int v) {
  return (*fns[which])(v);
}

int main(void) { return 0; }

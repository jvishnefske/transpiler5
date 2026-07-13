// M1 DCE regression: loop, break, and continue carry no traits and must
// survive --canonicalize unchanged.
// RUN: emitrust-opt --canonicalize %s | FileCheck %s

// CHECK-LABEL: emitrust.func @loop_survives_canonicalize
emitrust.func @loop_survives_canonicalize(%arg0: i1) {
  // CHECK: emitrust.loop {
  emitrust.loop {
    // CHECK: emitrust.if %{{.*}} {
    emitrust.if %arg0 {
      // CHECK: emitrust.break
      emitrust.break
    }
    // CHECK: emitrust.continue
    emitrust.continue
  }
  emitrust.return
}

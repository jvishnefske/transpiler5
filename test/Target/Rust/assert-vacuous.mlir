// FR-63 (clippy::assertions_on_constants): the importer's deterministic null
// guards render as `assert!(<flag>, "null pointer dereference")`; when the
// flag operand constant-folded to literal `true` the assertion can never
// fire, so the emitter drops the statement entirely -- emitting nothing is
// behavior-identical, and the rendered `assert!(true, ...)` is exactly what
// clippy flags. This golden pins the drop's precise boundary:
//   1. a constant-TRUE assert emits NOTHING, and a true constant read ONLY
//      by dropped asserts leaves no orphaned `let` behind (the crate header
//      denies unused_variables);
//   2. a constant-FALSE assert still renders -- it fires, a behavioral
//      panic that the byte-diff oracle observes;
//   3. a non-constant flag still renders -- it guards a real path;
//   4. a true constant SHARED with a surviving consumer still renders at
//      that consumer while the assert is dropped.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK-LABEL: fn vacuous_only() {
// CHECK-NEXT:  }
emitrust.func @vacuous_only() {
  %t = emitrust.constant <true> : i1
  emitrust.call_opaque "assert!"(%t) {args = [0 : index, "null pointer dereference"]} : (i1) -> ()
  emitrust.call_opaque "assert!"(%t) {args = [0 : index, "null pointer dereference"]} : (i1) -> ()
  emitrust.return
}

// CHECK-LABEL: fn constant_false() {
// CHECK-NEXT:    assert!(false, "null pointer dereference");
// CHECK-NEXT:  }
emitrust.func @constant_false() {
  %f = emitrust.constant <false> : i1
  emitrust.call_opaque "assert!"(%f) {args = [0 : index, "null pointer dereference"]} : (i1) -> ()
  emitrust.return
}

// CHECK-LABEL: fn dynamic_flag(v0: bool) {
// CHECK-NEXT:    assert!(v0, "null pointer dereference");
// CHECK-NEXT:  }
emitrust.func @dynamic_flag(%arg0: i1) {
  emitrust.call_opaque "assert!"(%arg0) {args = [0 : index, "null pointer dereference"]} : (i1) -> ()
  emitrust.return
}

// CHECK-LABEL: fn shared_true() {
// CHECK-NEXT:    consume(true);
// CHECK-NEXT:  }
emitrust.func @shared_true() {
  %t = emitrust.constant <true> : i1
  emitrust.call_opaque "assert!"(%t) {args = [0 : index, "null pointer dereference"]} : (i1) -> ()
  emitrust.call_opaque "consume"(%t) : (i1) -> ()
  emitrust.return
}

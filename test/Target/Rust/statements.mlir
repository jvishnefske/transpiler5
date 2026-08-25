// FR-6: Mutability discipline: let renders as immutable let, mut let as let mut, assign as plain assignment.
// Also covers call_opaque statement/let/tuple forms, select if-expression
// bindings, and verbatim lines.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK: // module-level marker
emitrust.verbatim "// module-level marker"

// FR-130 increment 2: NOTHING here is ever read, so the mut-marked binding
// drops together with its (already dead-store) assignment, and the parameter
// -- whose last emitted reader was that binding's initializer -- cascades to
// the `_`-prefixed spelling. The deferred `let _v1: i32;` survives on purpose:
// a deferred binding is deliberately out of scope for the drop, because its
// declaration and its initializing write render at two different program
// points. `let _v1: i32;` with no initializer and no reader is legal Rust --
// definite-assignment only errors on a USE.
// CHECK-LABEL: fn bindings(_v0: i32) {
// CHECK-NEXT:    let _v1: i32;
// CHECK-NEXT:  }
emitrust.func @bindings(%arg0: i32) {
  %0 = emitrust.let %arg0 : i32
  %1 = emitrust.let mut %arg0 : i32
  emitrust.assign %1 = %0 : i32
  emitrust.return
}

// The FR-6 trio this file exists to pin, on a binding that IS read so FR-130
// cannot drop it: an immutable `let`, a mut-marked `let mut`, and an
// assignment rendered as a PLAIN assignment (the value is not self-referential,
// so FR-63's compound-assign fold leaves it alone). Without this case the
// mutability discipline above would be pinned only by a function whose every
// binding is dead.
// CHECK-LABEL: fn bindings_live(v0: i32) -> i32 {
// CHECK-NEXT:    let mut [[M:v[0-9]+]]: i32 = v0;
// CHECK-NEXT:    let [[C:v[0-9]+]]: i32 = [[M]];
// CHECK-NEXT:    [[M]] = v0 * v0;
// CHECK-NEXT:    [[M]] + [[C]]
// CHECK-NEXT:  }
emitrust.func @bindings_live(%arg0: i32) -> i32 {
  %1 = emitrust.let mut %arg0 : i32
  %2 = emitrust.let %1 : i32
  %t = emitrust.mul %arg0, %arg0 : i32
  emitrust.assign %1 = %t : i32
  %sum = emitrust.add %1, %2 : i32
  emitrust.return %sum : i32
}

// CHECK-LABEL: fn calls(v0: i32, v1: i32) {
// CHECK-NEXT:    consume(v0);
// CHECK-NEXT:    let _v2: i32 = produce(v0, v1);
// CHECK-NEXT:    let (_v3, _v4): (i32, i64) = pair(v0);
// CHECK-NEXT:  }
emitrust.func @calls(%arg0: i32, %arg1: i32) {
  emitrust.call_opaque "consume"(%arg0) : (i32) -> ()
  %0 = emitrust.call_opaque "produce"(%arg0, %arg1) : (i32, i32) -> i32
  %1:2 = emitrust.call_opaque "pair"(%arg0) : (i32) -> (i32, i64)
  emitrust.return
}

// A select renders as a let binding of a Rust if expression.
// CHECK-LABEL: fn selects(v0: bool, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    let v3: i32 = if v0 { v1 } else { v2 };
// CHECK-NEXT:    if v0 { v3 } else { v1 }
// CHECK-NEXT:  }
emitrust.func @selects(%arg0: i1, %arg1: i32, %arg2: i32) -> i32 {
  %0 = emitrust.select %arg0, %arg1, %arg2 : i32
  %1 = emitrust.select %arg0, %0, %arg1 : i32
  emitrust.return %1 : i32
}

// CHECK-LABEL: fn raw_statement() {
// CHECK:         // inline marker
emitrust.func @raw_statement() {
  emitrust.verbatim "// inline marker"
  emitrust.return
}

// FR-6: Mutability discipline: let renders as immutable let, mut let as let mut, assign as plain assignment.
// Also covers call_opaque statement/let/tuple forms, select if-expression
// bindings, and verbatim lines.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK: // module-level marker
emitrust.verbatim "// module-level marker"

// CHECK-LABEL: fn bindings(v0: i32) {
// CHECK-NEXT:    let v1: i32 = v0;
// CHECK-NEXT:    let mut v2: i32 = v0;
// CHECK-NEXT:    v2 = v1;
// CHECK-NEXT:    return;
// CHECK-NEXT:  }
emitrust.func @bindings(%arg0: i32) {
  %0 = emitrust.let %arg0 : i32
  %1 = emitrust.let mut %arg0 : i32
  emitrust.assign %1 = %0 : i32
  emitrust.return
}

// CHECK-LABEL: fn calls(v0: i32, v1: i32) {
// CHECK-NEXT:    consume(v0);
// CHECK-NEXT:    let v2: i32 = produce(v0, v1);
// CHECK-NEXT:    let (v3, v4): (i32, i64) = pair(v0);
// CHECK-NEXT:    return;
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
// CHECK-NEXT:    let v4: i32 = if v0 { v3 } else { v1 };
// CHECK-NEXT:    return v4;
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

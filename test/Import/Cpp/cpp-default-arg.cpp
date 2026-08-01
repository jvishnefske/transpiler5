// RUN: emitrust-import-c %s | FileCheck %s

// A defaulted call argument (`add(5)` where `add(int a, int b = 10)`) surfaces
// at the call site as a `CXXDefaultArgExpr` standing in for the default value.
// The importer resolves it to that value, so a call omitting the argument
// materializes the default at the call site. It used to reject as
// "unsupported expression: CXXDefaultArgExpr" -- and, because the node's own
// source location is invalid, WITHOUT a file:line:col prefix.

int add(int a, int b = 10) {
  return a + b;
}

int use() {
  return add(5);
}

// CHECK: func.func @add(%{{.*}}: i32, %{{.*}}: i32) -> i32
// CHECK:      func.func @use_() -> i32
// CHECK:      %[[A:.*]] = arith.constant 5 : i32
// CHECK-NEXT: %[[B:.*]] = arith.constant 10 : i32
// CHECK-NEXT: %{{.*}} = call @add(%[[A]], %[[B]])

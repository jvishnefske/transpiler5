// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

int assignment_value(int x) {
  int y;
  int z;
  // The value of an assignment is the post-assignment value: both chained
  // stores see 5.
  y = (x = 5);
  z = (x += 2);
  return y + z;
}

// The value is re-loaded from the assigned place after the store.
// CHECK-LABEL: func.func @assignment_value
// CHECK: %[[C5:.*]] = arith.constant 5 : i32
// CHECK: memref.store %[[C5]], %[[X:.*]][]
// CHECK: %[[V:.*]] = memref.load %[[X]][]
// CHECK: memref.store %[[V]]
// SCF-LABEL: func.func @assignment_value

int postfix_prefix(int x) {
  // Postfix forms yield the original value, prefix forms the updated one.
  int a = x++;
  int b = x--;
  int c = ++x;
  int d = --x;
  return a * 1000 + b * 100 + c * 10 + d;
}

// CHECK-LABEL: func.func @postfix_prefix
// CHECK: arith.addi
// CHECK: arith.subi
// SCF-LABEL: func.func @postfix_prefix

int inside_expressions(int x, int y) {
  // Value-position updates nested inside larger expressions; the
  // side effects run exactly once each.
  int r = (x += 1) * (y = x + 2) + x++;
  return r + x + y;
}

// CHECK-LABEL: func.func @inside_expressions
// SCF-LABEL: func.func @inside_expressions

int in_condition(int x) {
  int n = 0;
  while ((x -= 1) > 0) {
    n++;
  }
  if (n++) {
    return n;
  }
  return 0;
}

// CHECK-LABEL: func.func @in_condition
// SCF-LABEL: func.func @in_condition

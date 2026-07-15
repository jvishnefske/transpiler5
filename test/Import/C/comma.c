// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

void tick(int *counter) {
  *counter = *counter + 1;
}

int value_position(int a) {
  int b;
  // The comma evaluates the left operand for its side effects only and
  // yields the right operand's value.
  b = (a = a + 1, a * 2);
  return b;
}

// CHECK-LABEL: func.func @value_position
// CHECK: arith.addi
// CHECK: memref.store
// CHECK: arith.muli
// SCF-LABEL: func.func @value_position

int statement_position(int a) {
  int calls = 0;
  // In statement position both operands run for effect; a void right
  // operand is fine here.
  tick(&calls), tick(&calls);
  a++, a *= 3;
  return a + calls;
}

// CHECK-LABEL: func.func @statement_position
// CHECK: call @tick
// CHECK: call @tick
// CHECK: arith.addi
// CHECK: arith.muli
// SCF-LABEL: func.func @statement_position

int in_for(int n) {
  int s = 0;
  int j = 0;
  // Comma expressions in for-init and for-increment clauses.
  for (int i = 0, k = (j = 1, 0); i < n; i++, j += 2) {
    s = s + i + k;
  }
  return s + j;
}

// CHECK-LABEL: func.func @in_for
// SCF-LABEL: func.func @in_for

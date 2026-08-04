// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

int printf(const char *, ...);

void tick(int *counter) {
  *counter = *counter + 1;
}

int cast_call(void) {
  int calls = 0;
  // A (void) cast of a call evaluates the call for its side effects and
  // discards the value; the call must survive.
  (void)tick(&calls);
  (void)printf("d=%d\n", calls);
  return calls;
}

// CHECK-LABEL: func.func @cast_call
// CHECK: call @tick
// CHECK: emitrust.call_opaque "println!"
// SCF-LABEL: func.func @cast_call

int cast_assign(int a) {
  // The discarded assignment's store must survive.
  (void)(a = a + 1);
  return a;
}

// CHECK-LABEL: func.func @cast_assign
// CHECK: arith.addi
// CHECK: memref.store
// SCF-LABEL: func.func @cast_assign

int cast_pure(int a, int b) {
  // A side-effect-free operand emits no code at all: the discarded sum
  // leaves nothing behind, only the return's own add remains.
  (void)(a * b);
  (void)a;
  return a + b;
}

// CHECK-LABEL: func.func @cast_pure
// CHECK-NOT: arith.muli
// CHECK: arith.addi
// CHECK-NOT: arith.muli
// SCF-LABEL: func.func @cast_pure

int comma_arm(int a) {
  int calls = 0;
  // A (void) cast as the left comma operand in value position: the side
  // effect runs, the right operand's value flows out.
  a = ((void)tick(&calls), calls + 10);
  return a;
}

// CHECK-LABEL: func.func @comma_arm
// CHECK: call @tick
// CHECK: arith.addi
// SCF-LABEL: func.func @comma_arm

void void_conditional(int c) {
  int x = 0;
  int y = 0;
  // A void-typed conditional in statement position (GNU shape: the arms
  // are cast to void) lowers as an if/else; only the selected arm's side
  // effects run and no value cell is materialized.
  c ? (void)(x = 1) : (void)(y = 2);
  printf("%d %d\n", x, y);
}

// CHECK-LABEL: func.func @void_conditional
// CHECK: cf.cond_br %{{.*}}, ^[[TRUE:bb[0-9]+]], ^[[FALSE:bb[0-9]+]]
// CHECK: ^[[TRUE]]:
// CHECK: memref.store
// CHECK: cf.br ^[[END:bb[0-9]+]]
// CHECK: ^[[FALSE]]:
// CHECK: memref.store
// CHECK: cf.br ^[[END]]
// SCF-LABEL: func.func @void_conditional

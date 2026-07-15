// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

int sum_down(int n) {
  int s = 0;
  do {
    s += n;
    n--;
  } while (n > 0);
  return s;
}

// The body block is entered unconditionally (it runs at least once), then
// the condition block branches back to the body or on to the exit.
// CHECK-LABEL: func.func @sum_down
// CHECK: cf.br ^[[BODY:bb[0-9]+]]
// CHECK: ^[[BODY]]:
// CHECK: arith.addi
// CHECK: cf.br ^[[COND:bb[0-9]+]]
// CHECK: ^[[COND]]:
// CHECK: arith.cmpi sgt
// CHECK: cf.cond_br %{{.*}}, ^[[BODY]], ^{{bb[0-9]+}}

// The lifted form is a structured loop, not leftover cf branches.
// SCF-LABEL: func.func @sum_down
// SCF-NOT: cf.br

int with_break_continue(int n) {
  int s = 0;
  do {
    if (n == 3) {
      n--;
      continue; // Branches to the condition block.
    }
    if (s > 50) {
      break; // Branches to the exit block.
    }
    s += n;
    n--;
  } while (n > 0);
  return s;
}

// CHECK-LABEL: func.func @with_break_continue
// SCF-LABEL: func.func @with_break_continue

int runs_once(void) {
  int c = 0;
  do {
    c++;
  } while (0);
  return c;
}

// CHECK-LABEL: func.func @runs_once
// SCF-LABEL: func.func @runs_once

int nested(int n) {
  int total = 0;
  do {
    int i = 0;
    do {
      total += 1;
      i++;
    } while (i < n);
    n--;
  } while (n > 0);
  return total;
}

// CHECK-LABEL: func.func @nested
// SCF-LABEL: func.func @nested

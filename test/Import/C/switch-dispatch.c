// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

// Switch bodies the structured lowering cannot shape (CTS-S2): case labels
// buried inside inner statements (Duff-adjacent), non-compound bodies, and
// statements before the first label. Each label becomes an ordinary block
// registered up front, the dispatch is one cf.switch to those targets, and
// the body is emitted in source order so fall-through — including into and
// out of a loop body — is plain block fall-into. The possibly irreducible
// result is absorbed by lift-cf-to-scf, exactly like goto into a loop.

int duff_like(int x, int n) {
  int r = 0;
  switch (x) {
  case 0:
    do {
      r = r + 1;
  case 1:
      r = r + 10;
    } while (--n > 0);
    break;
  default:
    r = 99;
  }
  return r;
}

// The dispatch targets case 1's block inside the do body directly; case 0
// enters at the top of the loop body, and the latch's back edge re-enters
// the loop body head, not the case block.
// CHECK-LABEL: func.func @duff_like
// CHECK: cf.switch %{{[0-9]+}} : i32, [
// CHECK-NEXT: default: ^[[DEF:bb[0-9]+]],
// CHECK-NEXT: 0: ^[[C0:bb[0-9]+]],
// CHECK-NEXT: 1: ^[[C1:bb[0-9]+]]
// CHECK-NEXT: ]
// CHECK: ^[[C0]]:
// CHECK-NEXT: cf.br ^[[BODY:bb[0-9]+]]
// CHECK: ^[[C1]]:
// CHECK: arith.addi
// CHECK: cf.br ^[[LATCH:bb[0-9]+]]
// CHECK: ^[[DEF]]:
// CHECK: memref.store
// CHECK: ^[[BODY]]:
// CHECK: arith.addi
// CHECK: cf.br ^[[C1]]
// CHECK: ^[[LATCH]]:
// CHECK: arith.cmpi sgt
// CHECK: cf.cond_br %{{[0-9]+}}, ^[[BODY]], ^bb{{[0-9]+}}

int noncompound(int x) {
  switch (x)
    case 3:
      return 7;
  return 1;
}

// A non-compound body: the single case is still a real dispatch target and
// the no-match path branches straight to the exit.
// CHECK-LABEL: func.func @noncompound
// CHECK: cf.switch %{{[0-9]+}} : i32, [
// CHECK-NEXT: default: ^[[NDEF:bb[0-9]+]],
// CHECK-NEXT: 3: ^[[NC3:bb[0-9]+]]
// CHECK-NEXT: ]
// CHECK: ^[[NC3]]:
// CHECK: return
// CHECK: ^[[NDEF]]:
// CHECK: return

int case_in_if(int x, int f) {
  int r = 0;
  switch (x) {
  case 0:
    if (f) {
  case -1:
      r = 5;
    } else {
  case 2:
      r = 6;
    }
  }
  return r;
}

// Case labels inside both arms of an if: each arm's label is a dispatch
// target, entering at case 0 still evaluates the if condition, and both
// arms merge and fall out of the switch. The -1 label keeps its i32 bit
// pattern in the dispatch (cf.switch prints case values unsigned).
// CHECK-LABEL: func.func @case_in_if
// CHECK: cf.switch %{{[0-9]+}} : i32, [
// CHECK-NEXT: default: ^bb{{[0-9]+}},
// CHECK-NEXT: 0: ^[[IC0:bb[0-9]+]],
// CHECK-NEXT: 4294967295: ^[[ICM1:bb[0-9]+]],
// CHECK-NEXT: 2: ^[[IC2:bb[0-9]+]]
// CHECK-NEXT: ]
// CHECK: ^[[IC0]]:
// CHECK: arith.cmpi ne
// CHECK: cf.cond_br
// CHECK: ^[[ICM1]]:
// CHECK: memref.store
// CHECK: ^[[IC2]]:
// CHECK: memref.store

int dead_prefix(int x) {
  switch (x) {
    {
      x = 1 + 1;
  case 1:
      return 5;
    }
  }
  return x;
}

// A statement before the first label is emitted into a dead block (only
// the dispatch enters the body) and erased with the unreachable prefix;
// the label buried in the inner compound is still a dispatch target.
// CHECK-LABEL: func.func @dead_prefix
// CHECK: cf.switch %{{[0-9]+}} : i32, [
// CHECK-NEXT: default: ^bb{{[0-9]+}},
// CHECK-NEXT: 1: ^[[PC1:bb[0-9]+]]
// CHECK-NEXT: ]
// CHECK: ^[[PC1]]:
// CHECK: return

// After mem2reg + lift-cf-to-scf no unstructured control flow remains,
// even for the irreducible jump-into-loop dispatch.
// SCF-LABEL: func.func @duff_like
// SCF-NOT: cf.switch
// SCF-NOT: cf.br
// SCF-NOT: cf.cond_br
// SCF: scf.while
// SCF: return
// SCF-LABEL: func.func @noncompound
// SCF-NOT: cf.switch
// SCF: return
// SCF-LABEL: func.func @case_in_if
// SCF-NOT: cf.switch
// SCF-NOT: cf.cond_br
// SCF: return
// SCF-LABEL: func.func @dead_prefix
// SCF-NOT: cf.switch
// SCF: return

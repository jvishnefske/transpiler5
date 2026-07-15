// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

int classify(int x) {
  int r = 0;
  switch (x) {
  case 0:
    r = 10;
    break;
  case 1:
    r = 11;
    // Falls through into case 2.
  case 2:
    r = r + 12;
    break;
  default:
    r = 99;
    break;
  }
  return r;
}

int sum_selected(int n) {
  int total = 0;
  for (int i = 0; i < n; ++i) {
    switch (i) {
    case 0:
      total += 1;
      break;
    default:
      total += 2;
      break;
    }
    if (total > 40) {
      break; // Exits the loop: the switch above is already closed.
    }
  }
  return total;
}

int nested(int a, int b) {
  int r = 0;
  switch (a) {
  case 1:
    switch (b) {
    case 1:
      r = 5;
      break;
    case 2:
      r = 8;
      break;
    default:
      r = 6;
      break;
    }
    break;
  case 2:
    r = 4;
    break;
  default:
    switch (b) {
    case 0:
      r = 9;
      break;
    default:
      r = 7;
      break;
    }
    break;
  }
  return r;
}

// Raw import: one block per label position plus an exit block, wired up by
// a cf.switch on the controlling value. The fallthrough from case 1 into
// case 2 is a plain branch; every break branches to the exit block.
// CHECK-LABEL: func.func @classify
// CHECK-SAME: (%{{.*}}: i32) -> i32
// CHECK: cf.switch %{{[0-9]+}} : i32, [
// CHECK-NEXT: default: ^[[DEF:bb[0-9]+]],
// CHECK-NEXT: 0: ^[[C0:bb[0-9]+]],
// CHECK-NEXT: 1: ^[[C1:bb[0-9]+]],
// CHECK-NEXT: 2: ^[[C2:bb[0-9]+]]
// CHECK-NEXT: ]
// CHECK: ^[[C0]]:
// CHECK: memref.store
// CHECK: cf.br ^[[EXIT:bb[0-9]+]]
// CHECK: ^[[C1]]:
// CHECK: memref.store
// CHECK: cf.br ^[[C2]]
// CHECK: ^[[C2]]:
// CHECK: arith.addi
// CHECK: cf.br ^[[EXIT]]
// CHECK: ^[[DEF]]:
// CHECK: memref.store
// CHECK: cf.br ^[[EXIT]]
// CHECK: ^[[EXIT]]:
// CHECK: return

// A switch nested in a loop: the loop keeps its own exit for the trailing
// break while the switch gets its own cf.switch and exit wiring. The
// switch-exit block (with the total > 40 test) is appended after the
// loop-exit block, so the tail checks must not assume printed block order.
// CHECK-LABEL: func.func @sum_selected
// CHECK: cf.switch %{{[0-9]+}} : i32, [
// CHECK-NEXT: default: ^bb{{[0-9]+}},
// CHECK-NEXT: 0: ^bb{{[0-9]+}}
// CHECK-NEXT: ]
// CHECK-DAG: arith.cmpi sgt
// CHECK-DAG: return

// A switch nested inside another switch's case arm (and inside its default
// arm) is legal C: the inner labels belong to the inner switch, so the
// Duff's-device guard must not mistake them for misplaced outer labels.
// Each of the three switches gets its own cf.switch dispatch.
// CHECK-LABEL: func.func @nested
// CHECK: cf.switch %{{[0-9]+}} : i32, [
// CHECK-NEXT: default: ^[[ODEF:bb[0-9]+]],
// CHECK-NEXT: 1: ^[[OC1:bb[0-9]+]],
// CHECK-NEXT: 2: ^bb{{[0-9]+}}
// CHECK-NEXT: ]
// CHECK: ^[[OC1]]:
// CHECK: cf.switch %{{[0-9]+}} : i32, [
// CHECK-NEXT: default: ^bb{{[0-9]+}},
// CHECK-NEXT: 1: ^bb{{[0-9]+}},
// CHECK-NEXT: 2: ^bb{{[0-9]+}}
// CHECK-NEXT: ]
// CHECK: ^[[ODEF]]:
// CHECK: cf.switch %{{[0-9]+}} : i32, [
// CHECK-NEXT: default: ^bb{{[0-9]+}},
// CHECK-NEXT: 0: ^bb{{[0-9]+}}
// CHECK-NEXT: ]
// CHECK: return

// After mem2reg + lift-cf-to-scf the memory cells disappear and the switch
// is recovered as structured scf.index_switch.
// SCF-LABEL: func.func @classify
// SCF-NOT: memref.alloca
// SCF: scf.index_switch
// SCF: return
// SCF-NOT: cf.switch

// SCF-LABEL: func.func @sum_selected
// SCF-NOT: memref.alloca
// SCF: scf.while
// SCF: scf.index_switch
// SCF: return
// SCF-NOT: cf.switch

// The nested switches lift to three structured scf.index_switch ops (the
// outer dispatch plus one per inner switch).
// SCF-LABEL: func.func @nested
// SCF-NOT: memref.alloca
// SCF: scf.index_switch
// SCF: scf.index_switch
// SCF: scf.index_switch
// SCF: return
// SCF-NOT: cf.switch

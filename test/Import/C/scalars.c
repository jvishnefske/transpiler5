// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

int compute(int a, int b) {
  int sum = 0;
  if (a < b) {
    sum = a + b;
  } else {
    sum = a - b;
  }
  while (sum > 100) {
    sum = sum - 7;
  }
  for (int i = 0; i < 10; ++i) {
    if (i == 3) {
      continue;
    }
    if (i > 8) {
      break;
    }
    sum += i * 2;
  }
  return sum;
}

// Raw import: scalar locals are rank-0 memref cells and control flow is
// explicit cf branches.
// CHECK-LABEL: func.func @compute
// CHECK-SAME: (%{{.*}}: i32, %{{.*}}: i32) -> i32
// CHECK: memref.alloca() : memref<i32>
// CHECK: memref.store
// CHECK: arith.cmpi slt
// CHECK: cf.cond_br
// CHECK: arith.addi
// CHECK: arith.subi
// CHECK: arith.cmpi sgt
// CHECK: arith.cmpi eq
// The compound-assign block is appended after the return block, so the tail
// checks must not assume printed block order.
// CHECK-DAG: arith.muli
// CHECK-DAG: return

// After mem2reg + lift-cf-to-scf the cells and branches disappear into
// structured control flow.
// SCF-LABEL: func.func @compute
// SCF-NOT: memref.alloca
// SCF: scf.if
// SCF: scf.while
// SCF: return
// SCF-NOT: cf.cond_br

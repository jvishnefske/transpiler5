// RUN: emitrust-import-c %s | FileCheck %s

// CTS-P7: one pointer ranging over several distinct objects
// (`p = &x; ... p = &y;`). The region keeps every base under the
// enum-of-bases model: each pointer carries a promotable rank-0
// memref<i32> base-discriminant cell alongside its i64 cursor cell
// (a tagged (base-index, cursor) pair). An address binding stores the
// bound base's index, `p = q` copies the source discriminant, and every
// dereference dispatches on the discriminant — a match over the closed
// set of bases in which each arm touches exactly one base (each variant
// names a disjoint region, so the disjoint-region invariant holds and
// the objects stay independently addressable). Same-region equality
// compares (discriminant, cursor) pairs.

// Rebinding between two arrays with reads, writes, and cursor
// arithmetic through both phases (the 00077 shape over locals).
int rebind_arrays(void) {
  int a[4];
  int b[4];
  int *p;
  int first;
  a[0] = 1;
  b[0] = 2;
  p = a;
  *p = 10;
  first = p[0];
  p = b + 1;
  p--;
  *p = 20;
  return first + p[0];
}
// CHECK-LABEL: func.func @rebind_arrays
// CHECK: %[[A:[0-9]+]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK: %[[B:[0-9]+]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
// `p = a` stores discriminant 0 and cursor 0.
// CHECK: memref.store %{{.*}}, %[[DISC:alloca[_0-9]*]][] : memref<i32>
// CHECK-NEXT: memref.store %{{.*}}, %[[CUR:alloca[_0-9]*]][] : memref<i64>
// `*p = 10` dispatches on the discriminant.
// CHECK: %[[CV:[0-9]+]] = memref.load %[[CUR]][] : memref<i64>
// CHECK: %[[DV:[0-9]+]] = memref.load %[[DISC]][] : memref<i32>
// CHECK: %[[HIT:[0-9]+]] = arith.cmpi eq, %[[DV]], %{{.*}} : i32
// CHECK: cf.cond_br %[[HIT]]
// The write flush assigns into exactly one base per arm.
// CHECK: emitrust.subscript %[[A]][%[[CV]]]
// CHECK: emitrust.assign
// CHECK: cf.br
// CHECK: emitrust.subscript %[[B]][%[[CV]]]
// CHECK: emitrust.assign
// CHECK: cf.br
// `p = b + 1` rebinds: discriminant 1, cursor 1.
// CHECK: memref.store %c1_i32{{[_0-9]*}}, %[[DISC]][] : memref<i32>
// `p--` is plain cursor arithmetic on the active base.
// CHECK: arith.subi
// CHECK: memref.store %{{.*}}, %[[CUR]][] : memref<i64>

// Two pointers over two scalar objects with equality before and after a
// discriminant copy (the 00172 shape): all bases are degenerate, so the
// pointers carry discriminant cells only and equality compares the
// discriminants (the cursors are constant zero).
int scalar_rebind(void) {
  int x;
  int y;
  int *d;
  int *e;
  int same;
  int now;
  d = &x;
  e = &y;
  x = 3;
  y = 4;
  same = (d == e);
  d = e;
  now = (d == e);
  return *d + same + now;
}
// CHECK-LABEL: func.func @scalar_rebind
// CHECK: %[[X:[0-9]+]] = emitrust.variable : !emitrust.lvalue<i32>
// CHECK: %[[Y:[0-9]+]] = emitrust.variable : !emitrust.lvalue<i32>
// `d = &x` stores index 0; `e = &y` stores index 1 in e's own cell.
// CHECK: memref.store %c0_i32{{[_0-9]*}}, %[[DD:alloca[_0-9]*]][] : memref<i32>
// CHECK: memref.store %c1_i32{{[_0-9]*}}, %[[ED:alloca[_0-9]*]][] : memref<i32>
// `d == e` compares the discriminants (and the constant-zero cursors).
// CHECK: %[[DL:[0-9]+]] = memref.load %[[DD]][] : memref<i32>
// CHECK: %[[EL:[0-9]+]] = memref.load %[[ED]][] : memref<i32>
// CHECK: %[[SAME:[0-9]+]] = arith.cmpi eq, %[[DL]], %[[EL]] : i32
// CHECK: arith.andi %[[SAME]]
// `d = e` copies the discriminant.
// CHECK: %[[COPY:[0-9]+]] = memref.load %[[ED]][] : memref<i32>
// CHECK: memref.store %[[COPY]], %[[DD]][] : memref<i32>
// `*d` dispatches to the scalar places themselves (no cursor).
// CHECK: cf.cond_br
// CHECK: emitrust.load %[[X]] : (!emitrust.lvalue<i32>) -> i32
// CHECK: emitrust.load %[[Y]] : (!emitrust.lvalue<i32>) -> i32

// The discriminant is ordinary promotable state, so a binding
// established in one iteration flows into the next (loop-carried).
int loop_carried(int n) {
  int a[4];
  int b[4];
  int *p;
  int i;
  a[0] = 0;
  b[0] = 0;
  p = a;
  for (i = 0; i < n; i++) {
    *p = *p + 1;
    p = b;
  }
  return a[0] + b[0];
}
// CHECK-LABEL: func.func @loop_carried
// The compound read-modify-write dispatches twice on one loaded
// discriminant (read staging, then write flush); the (cursor,
// discriminant) pair loads together at the dereference.
// CHECK: cf.cond_br
// CHECK: memref.load %{{.*}}[] : memref<i64>
// CHECK-NEXT: %[[LDV:[0-9]+]] = memref.load %[[LDISC:alloca[_0-9]*]][] : memref<i32>
// CHECK: arith.cmpi eq, %[[LDV]], %{{.*}} : i32
// CHECK: cf.cond_br
// CHECK: arith.addi
// CHECK: arith.cmpi eq, %[[LDV]], %{{.*}} : i32
// CHECK: cf.cond_br
// `p = b` inside the body stores discriminant 1 for the next iteration.
// CHECK: memref.store %c1_i32{{[_0-9]*}}, %[[LDISC]][] : memref<i32>

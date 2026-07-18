// RUN: emitrust-import-c %s | FileCheck %s

// CTS-P10 (00181, Towers of Hanoi shape): pointer parameters whose
// interprocedural class is backed ONLY by mutable globals of one element
// type lower to a cell-slice: the parameter becomes
// `!emitrust.ref<!emitrust.cell_slice<T>>` (rendered `&[Cell<T>]`), the
// call site wraps the call in `emitrust.global_cells` regions (one per
// distinct global argument, nested in argument order, leftmost
// outermost; each region's entry block argument is the global's borrowed
// cell-slice), and element accesses in the callee are `emitrust.cell_get`
// / `emitrust.cell_set` on the reference itself. Cell-slices are shared
// references, so they are freely duplicable: permuted recursive
// forwarding needs no reborrow discipline, and a callee that reads the
// global directly (staged copy through the Cell) between or during
// cell-slice mutations observes every write (Cell get/set and
// global_load/global_store hit the same thread-local Cell).

int A[4];
int B[4];
int C[4];

// Reads the global directly: the existing staged-copy machinery, no
// cell-slice involvement.
int reader(void) {
  int s = 0;
  int i;
  for (i = 0; i < 4; i++)
    s = s + A[i];
  return s;
}
// CHECK-LABEL: func.func @reader
// CHECK: emitrust.global_load @A : !emitrust.array<4xi32>

// A subscripted parameter whose only bases are globals becomes a
// cell-slice; element reads are cell_get on the reference at an i64
// index.
int sum4(int *a) {
  int s = 0;
  int i;
  for (i = 0; i < 4; i++)
    s = s + a[i];
  return s;
}
// CHECK-LABEL: func.func @sum4
// CHECK-SAME: (%{{[^ :,)]+}}: !emitrust.ref<!emitrust.cell_slice<i32>>)
// CHECK: emitrust.cell_get %{{.*}}[%{{.*}}] : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64) -> i32

// Writes through both parameters, then calls a function that reads the
// global A directly mid-call: the coherence trap. Direct reads must see
// the cell_set writes because both go through the same Cell.
void move2(int *src, int *dst) {
  int v = src[0];
  src[0] = 0;
  dst[1] = v;
  reader();
}
// CHECK-LABEL: func.func @move2
// CHECK-SAME: (%[[SRC:[^ :,)]+]]: !emitrust.ref<!emitrust.cell_slice<i32>>, %[[DST:[^ :,)]+]]: !emitrust.ref<!emitrust.cell_slice<i32>>)
// CHECK: emitrust.cell_get %[[SRC]][%{{.*}}] : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64) -> i32
// CHECK: emitrust.cell_set %[[SRC]][%{{.*}}], %{{.*}} : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64, i32) -> ()
// CHECK: emitrust.cell_set %[[DST]][%{{.*}}], %{{.*}} : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64, i32) -> ()
// CHECK: call @reader()

// Plain forwarding: an unwalked cell-slice parameter (cursor never
// moves) is passed onward as the same SSA value — shared references are
// freely duplicable, no reborrow op.
int through(int *q) {
  return sum4(q);
}
// CHECK-LABEL: func.func @through
// CHECK-SAME: (%[[Q:[^ :,)]+]]: !emitrust.ref<!emitrust.cell_slice<i32>>)
// CHECK: call @sum4(%[[Q]]) : (!emitrust.ref<!emitrust.cell_slice<i32>>) -> i32

// The Hanoi recursion shape: the function passes its own cell-slice
// parameters along PERMUTED. Sound because the parameters are shared
// references into Cells: permuted aliasing needs no exclusivity.
void shuffle(int n, int *s, int *d, int *t) {
  if (n == 0)
    return;
  shuffle(n - 1, s, t, d);
  move2(s, d);
  shuffle(n - 1, t, d, s);
}
// CHECK-LABEL: func.func @shuffle
// CHECK-SAME: (%{{[^ :,)]+}}: i32, %[[S:[^ :,)]+]]: !emitrust.ref<!emitrust.cell_slice<i32>>, %[[D:[^ :,)]+]]: !emitrust.ref<!emitrust.cell_slice<i32>>, %[[T:[^ :,)]+]]: !emitrust.ref<!emitrust.cell_slice<i32>>)
// CHECK: call @shuffle(%{{.*}}, %[[S]], %[[T]], %[[D]])
// CHECK: call @move2(%[[S]], %[[D]])
// CHECK: call @shuffle(%{{.*}}, %[[T]], %[[D]], %[[S]])

int main(void) {
  int i;
  int r;
  for (i = 0; i < 4; i++)
    A[i] = i + 1;
  // One global argument: a single global_cells region borrows A's
  // cell-slice for the duration of the call.
  r = sum4(A);
  // Two different globals in one call: nested global_cells regions
  // (A outer, B inner — argument order).
  move2(A, B);
  // A direct global read between the mutating calls sees the writes.
  r = r + reader();
  // Three globals, permuted recursion inside the callee.
  shuffle(2, A, B, C);
  r = r + through(A);
  return r;
}
// CHECK-LABEL: func.func @c_main
// CHECK: emitrust.global_cells @A
// CHECK: call @sum4(%{{.*}}) : (!emitrust.ref<!emitrust.cell_slice<i32>>) -> i32
// CHECK: emitrust.global_cells @A
// CHECK: emitrust.global_cells @B
// CHECK: call @move2(%{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.cell_slice<i32>>, !emitrust.ref<!emitrust.cell_slice<i32>>) -> ()
// CHECK: call @reader()
// CHECK: emitrust.global_cells @A
// CHECK: emitrust.global_cells @B
// CHECK: emitrust.global_cells @C
// CHECK: call @shuffle(
// CHECK: emitrust.global_cells @A
// CHECK: call @through(

// RUN: emitrust-import-c %s | FileCheck %s

// FR: Phase-1b pointer parameters. Each pointer parameter is classified
// from the definition's body: a parameter that is only dereferenced or
// arrowed stays a scalar reference `!emitrust.mut_ref<T>` (the Phase-1a
// behavior), while a parameter that is subscripted, walked, compared,
// reassigned, or passed onward becomes `!emitrust.mut_ref<!emitrust.slice<T>>`,
// dereferenced once in the entry block into the region base place of an
// ordinary (base, i64 cursor) decomposition. Call sites reborrow the
// argument's region base: a decayed array becomes `emitrust.slice_of` at
// cursor 0, `&arr[i]` at cursor i, a decomposed pointer at its current
// cursor (reslicing through another slice parameter composes), and a
// scalar-reference target receives `emitrust.addr_of` of the designated
// element.

// A deref-only parameter keeps the Phase-1a scalar-reference shape.
void set_first(int *v) {
  *v = 7;
}
// CHECK-LABEL: func.func @set_first
// CHECK-SAME: (%[[V:.*]]: !emitrust.mut_ref<i32>)
// CHECK: emitrust.deref %[[V]] : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
// CHECK-NOT: !emitrust.slice

// A subscripted parameter becomes a slice: one entry-block deref
// establishes the base place, and the cursor cell initializes to zero.
int sum(int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    s = s + a[i];
  }
  return s;
}
// CHECK-LABEL: func.func @sum
// CHECK-SAME: (%[[A:.*]]: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.*}}: i32)
// CHECK: %[[CUR:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[BASE:.*]] = emitrust.deref %[[A]] : (!emitrust.mut_ref<!emitrust.slice<i32>>) -> !emitrust.lvalue<!emitrust.slice<i32>>
// CHECK: %[[ZERO:.*]] = arith.constant 0 : i64
// CHECK: memref.store %[[ZERO]], %[[CUR]][] : memref<i64>
//   a[i] subscripts the slice base at cursor + i.
// CHECK: %[[IDX:.*]] = arith.addi
// CHECK: emitrust.subscript %[[BASE]][%[[IDX]]] : (!emitrust.lvalue<!emitrust.slice<i32>>, i64) -> !emitrust.lvalue<i32>

// Walking a slice parameter updates its cursor cell like a pointer local.
int walk(int *p, int n) {
  int s = 0;
  while (n > 0) {
    s = s + *p;
    p++;
    n--;
  }
  return s;
}
// CHECK-LABEL: func.func @walk
// CHECK-SAME: (%[[P:.*]]: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.*}}: i32)
// CHECK: %[[WCUR:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[WBASE:.*]] = emitrust.deref %[[P]]
//   *p subscripts at the current cursor; p++ bumps the cursor cell.
// CHECK: %[[C:.*]] = memref.load %[[WCUR]][] : memref<i64>
// CHECK: emitrust.subscript %[[WBASE]][%[[C]]]
// CHECK: %[[C2:.*]] = memref.load %[[WCUR]][] : memref<i64>
// CHECK: %[[NEXT:.*]] = arith.addi %[[C2]], %{{.*}} : i64
// CHECK: memref.store %[[NEXT]], %[[WCUR]][] : memref<i64>

// Passing a slice parameter onward reslices the deref'd base place at the
// parameter's current cursor (reslicing composes).
int through(int *q, int n) {
  return sum(q, n);
}
// CHECK-LABEL: func.func @through
// CHECK-SAME: (%[[Q:.*]]: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.*}}: i32)
// CHECK: %[[TBASE:.*]] = emitrust.deref %[[Q]]
// CHECK: %[[TCUR:.*]] = memref.load %{{.*}}[] : memref<i64>
// CHECK: %[[RESLICE:.*]] = emitrust.slice_of mut %[[TBASE]][%[[TCUR]]] : (!emitrust.lvalue<!emitrust.slice<i32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
// CHECK: call @sum(%[[RESLICE]], %{{.*}})

// A forward declaration in the same TU classifies from the later
// definition, so prototype and definition agree on the slice shape.
int tail_item(int *a, int n);
// CHECK-LABEL: func.func @c_main
int main(void) {
  int arr[5];
  for (int i = 0; i < 5; i++) {
    arr[i] = i;
  }
  int x = 1;
  // CHECK: %[[ARR:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<5xi32>>
  // &x to a scalar-reference parameter keeps the historical addr_of path.
  // CHECK: %[[XREF:.*]] = emitrust.addr_of mut %{{.*}} : (!emitrust.lvalue<i32>) -> !emitrust.mut_ref<i32>
  // CHECK: call @set_first(%[[XREF]])
  set_first(&x);
  // Array decay to a slice parameter: slice_of at cursor 0. The value
  // argument (5) is materialized before the borrow-producing argument.
  // CHECK: %[[N5:.*]] = arith.constant 5 : i32
  // CHECK: %[[S0:.*]] = emitrust.slice_of mut %[[ARR]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<5xi32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  // CHECK: call @sum(%[[S0]], %[[N5]])
  int t = sum(arr, 5);
  // &arr[2] to a slice parameter: slice_of at cursor 2.
  // CHECK: emitrust.slice_of mut %[[ARR]][%{{.*}}]
  // CHECK: call @sum(
  t = t + sum(&arr[2], 3);
  // CHECK: call @walk(
  t = t + walk(arr, 5);
  // CHECK: call @through(
  t = t + through(arr, 5);
  // A decomposed pointer local passed to a slice parameter reslices its
  // base at the pointer's current cursor.
  int *p = arr;
  p++;
  // CHECK: %[[PS:.*]] = emitrust.slice_of mut %[[ARR]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<5xi32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  // CHECK: call @sum(%[[PS]], %{{.*}})
  t = t + sum(p, 4);
  // A decomposed pointer local passed to a scalar-reference parameter
  // borrows the designated element.
  int *e = &arr[1];
  // CHECK: %[[ELT:.*]] = emitrust.subscript %[[ARR]][%{{.*}}]
  // CHECK: %[[EREF:.*]] = emitrust.addr_of mut %[[ELT]] : (!emitrust.lvalue<i32>) -> !emitrust.mut_ref<i32>
  // CHECK: call @set_first(%[[EREF]])
  set_first(e);
  // Forward-declared slice callee: the call uses the definition's shape.
  // CHECK: call @tail_item(
  t = t + tail_item(arr, 5);
  return t + x;
}

int tail_item(int *a, int n) {
  return a[n - 1];
}
// CHECK-LABEL: func.func @tail_item
// CHECK-SAME: (%{{.*}}: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.*}}: i32)

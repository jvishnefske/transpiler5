// RUN: emitrust-import-c %s | FileCheck %s

// C99-36: array parameters with decay semantics. Every bracketed
// parameter form — unsized `a[]`, sized `a[40]`, the C99 minimum-length
// form `a[static 4]`, and the qualifier forms `a[const]` / `a[volatile]`
// — adjusts to a pointer in clang's AST (C99 6.7.5.3p7), so each one
// rides the ordinary Phase-1b pointer-parameter classification:
// subscripted use is a slice reference, deref-only use a scalar
// reference. The bracket qualifiers land on the POINTER object itself
// (`int *const` / `int *volatile`), which the decomposition erases, so
// they are accepted and ignored.

/* Unsized array parameter, subscripted in a loop: slice. */
int sum(int a[], int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s = s + a[i];
  return s;
}

/* Sized array parameter (the 40 is documentation only in C): slice. */
int first(int a[40]) {
  return a[0];
}

/* C99 `static 4` minimum-length guarantee: decays identically. */
int head4(int a[static 4]) {
  return a[0] + a[3];
}

/* `const` inside the brackets const-qualifies the decayed pointer
   (`int *const a`); the pointer is decomposed away, so nothing changes. */
int pick(int a[const], int i) {
  return a[i];
}

/* Deref-only use of a `static 1` array parameter: scalar reference. */
void bump(int a[static 1]) {
  *a = *a + 1;
}

/* `volatile` inside the brackets volatile-qualifies the decayed POINTER
   (`int *volatile a`), not the pointee; the pointer object is decomposed
   away, so the qualifier is accepted and ignored. */
void vbump(int a[volatile]) {
  *a = *a + 1;
}

int drive(void) {
  /* 40 elements keeps the array above the Phase-4 owner-promotion limit,
     pinning the plain Phase-1b slice lowering. */
  int arr[40];
  for (int i = 0; i < 40; i++)
    arr[i] = i;
  int x = 5;
  bump(&x);
  vbump(&x);
  return sum(arr, 40) + first(arr) + head4(arr) + pick(arr, 7) + x;
}

// Subscripted forms all classify as slice parameters.
// CHECK-LABEL: func.func @sum
// CHECK-SAME: (%[[A:.*]]: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.*}}: i32)
// CHECK: emitrust.deref %[[A]] : (!emitrust.mut_ref<!emitrust.slice<i32>>) -> !emitrust.lvalue<!emitrust.slice<i32>>
// CHECK: emitrust.subscript

// CHECK-LABEL: func.func @first
// CHECK-SAME: (%{{.*}}: !emitrust.mut_ref<!emitrust.slice<i32>>)
// CHECK: emitrust.subscript

// CHECK-LABEL: func.func @head4
// CHECK-SAME: (%{{.*}}: !emitrust.mut_ref<!emitrust.slice<i32>>)
// CHECK: emitrust.subscript

// CHECK-LABEL: func.func @pick
// CHECK-SAME: (%{{.*}}: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.*}}: i32)
// CHECK: emitrust.subscript

// Deref-only forms stay scalar references; the bracket qualifier and the
// static length leave no trace.
// CHECK-LABEL: func.func @bump
// CHECK-SAME: (%[[V:.*]]: !emitrust.mut_ref<i32>)
// CHECK: emitrust.deref %[[V]] : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>

// CHECK-LABEL: func.func @vbump
// CHECK-SAME: (%{{.*}}: !emitrust.mut_ref<i32>)
// CHECK: emitrust.deref

// Call sites: the local array reslices for slice parameters, the scalar
// borrows for the deref-only callees.
// CHECK-LABEL: func.func @drive
// CHECK: emitrust.addr_of
// CHECK: call @bump
// CHECK: emitrust.addr_of
// CHECK: call @vbump
// CHECK: emitrust.slice_of
// CHECK: call @sum
// CHECK: emitrust.slice_of
// CHECK: call @first
// CHECK: emitrust.slice_of
// CHECK: call @head4
// CHECK: emitrust.slice_of
// CHECK: call @pick

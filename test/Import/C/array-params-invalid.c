// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/multidim.c 2>&1 | FileCheck %s --check-prefix=MULTIDIM
// RUN: not emitrust-import-c %t/ptr-array.c 2>&1 | FileCheck %s --check-prefix=PTRARRAY

// C99-36 exclusions. Array parameters decay to pointers, so the decayed
// forms inherit the pointer-parameter boundaries with located
// diagnostics: a multidimensional array parameter decays to a
// pointer-to-array, which has no slice shape (two caller matrices keep
// the class multi-base, so the Phase-4 owner promotion cannot absorb it
// and the Phase-1b slice path is what runs), and an array-of-pointers
// parameter decays to a pointer-to-pointer, which has no decomposed
// representation (CTS-P5 keeps the general rejection).

//--- multidim.c
// MULTIDIM: multidim.c:2:16: error: unsupported: slice parameter element type '!emitrust.array<4xi32>'
int rowget(int a[][4], int i, int j) {
  return a[i][j];
}

int drive(void) {
  int m[2][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}};
  int n[2][4] = {{9, 8, 7, 6}, {5, 4, 3, 2}};
  return rowget(m, 1, 2) + rowget(n, 0, 3);
}

//--- ptr-array.c
// PTRARRAY: ptr-array.c:2:17: error: unsupported: pointer-to-pointer parameter
int deref0(int *a[]) {
  return *a[0];
}

int drive(void) {
  int x = 7;
  int *p = &x;
  return deref0(&p);
}

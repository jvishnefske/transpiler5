// W3.1 multi-TU gate oracle (G6 NEGATIVE): unlike
// multi-tu-gate-g6-cellslice-fn-external.c (whose companion calls `sum4`
// on its OWN internal global — genuinely sound once whole-program merge
// exists), this file's companion calls the SAME externally visible
// `sum4` with a LOCAL array. Even after W3.2 relaxes the owning-function
// internal-linkage check, a sound whole-program merge must still see
// this call site and keep `sum4`'s parameter off the cell-slice path (a
// local has no Cell to back `&[Cell<T>]`). This test pins TODAY's
// rejection (the same "historical" staged-copy wording), which must
// survive unchanged through W3.2.
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-gate-g6-cellslice-fn-negative-other.c 2>&1 | FileCheck %s

static int A[4];

int sum4(int *a) {
  int s = 0;
  int i;
  for (i = 0; i < 4; i++)
    s = s + a[i];
  return s;
}

int main(void) {
  int i;
  for (i = 0; i < 4; i++)
    A[i] = i + 1;
  return sum4(A);
}

// CHECK: multi-tu-gate-g6-cellslice-fn-negative.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer into a global variable to a function

// REQUIRES: cargo
// CTS-S4 / C99-41 multi-dimensional arrays, differential end-to-end test:
// transpile to a cargo crate, build it, and compare its stdout against the
// natively compiled C program. Exercises the 00130/00151 shapes: a 3-D
// file-scope array with designated, partial, and zero-filled initializers;
// a partially initialized 2-D local; writes through both index levels with
// data-dependent indices (so the differential run exercises the row-major
// subscript lowering at runtime, not just constant folding); a row pointer
// (`char (*)[4]`) subscripted at both levels; and a scalar pointer taken
// with `&arr[i][j]` and dereferenced. All indices stay in bounds; the
// program has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/arrays_multidim > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int g[2][3][5] = {
    {{0, 0, 3, 5}, {1, [3] = 6, 7}},
    {{1, 2}, {[4] = 7}},
};

int main(void) {
  /* Designated, partial, and zero-filled file-scope initialization. */
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 5; ++k)
        printf("g[%d][%d][%d]=%d\n", i, j, k, g[i][j][k]);

  /* Partially initialized 2-D local: the second row zero-fills past 5. */
  int a[3][4] = {{1, 2}, {5}};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 4; ++j)
      printf("a[%d][%d]=%d\n", i, j, a[i][j]);

  /* Writes through both index levels with data-dependent indices. */
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 4; ++j)
      a[i][j] = a[i][j] + i * 10 + j;
  int i = g[0][0][2] % 3;  /* 3 % 3 == 0, computed at runtime */
  int j = g[0][1][3] % 4;  /* 6 % 4 == 2 */
  a[i][j] = 77;
  a[i + 2][j + 1] = a[i][j] + 1;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c)
      printf("a2[%d][%d]=%d\n", r, c, a[r][c]);

  /* Row pointer and scalar pointer into a 2-D array (00130 shapes). */
  char arr[2][4];
  char (*p)[4];
  char *q;
  for (int r = 0; r < 2; ++r)
    for (int c = 0; c < 4; ++c)
      arr[r][c] = (char)(r * 4 + c);
  p = arr;
  q = &arr[i + 1][j + 1];  /* data-dependent flat cursor: row 1, col 3 */
  arr[1][3] = 42;
  printf("q=%d\n", *q);
  printf("p=%d\n", p[i + 1][j]);
  p[i][j + 1] = 9;
  printf("arr[0][3]=%d\n", arr[0][3]);
  return 0;
}

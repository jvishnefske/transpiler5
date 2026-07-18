// REQUIRES: cargo
// CTS-P10 (00181 mini-Hanoi), differential end-to-end test: global
// arrays passed as pointer parameters lower to `&[Cell<i32>]` cell
// slices borrowed inside nested `.with` accessor closures. The risky
// seams this drives adversarially: recursive calls that forward the
// same cell-slice parameters PERMUTED (source/dest/spare rotate every
// level); a direct-global-read printer (PrintAll) called from INSIDE
// Move while both cell-slice borrows are live and from main between
// moves — every cell_set must be visible to the staged direct reads
// because both hit the same thread-local Cell; a data-dependent disc
// count so nothing folds; and Move's return value flowing out of a
// call that sits under the .with nesting. Byte-identical stdout and
// exit code against the natively compiled program are required, the
// crate must borrow via as_slice_of_cells with no unsafe.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name hanoi_cells --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/hanoi_cells > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: grep -F ".as_slice_of_cells()" %t.crate/src/main.rs
// RUN: grep -F "&[std::cell::Cell<i32>]" %t.crate/src/main.rs
// RUN: grep -F ".with(" %t.crate/src/main.rs
// RUN: not grep "unsafe" %t.crate/src/main.rs

int printf(const char *, ...);

int A[4];
int B[4];
int C[4];

/* Reads all three globals directly: called mid-Move while the caller's
   cell-slice borrows are live, and from main between moves. */
void PrintAll(void) {
  int i;
  printf("A:");
  for (i = 0; i < 4; i++)
    printf(" %d", A[i]);
  printf("\n");
  printf("B:");
  for (i = 0; i < 4; i++)
    printf(" %d", B[i]);
  printf("\n");
  printf("C:");
  for (i = 0; i < 4; i++)
    printf(" %d", C[i]);
  printf("\n----\n");
}

/* Move the leftmost nonzero element of source to dest, leave 0 behind;
   prints the full state (direct global reads) before returning. */
int Move(int *source, int *dest) {
  int i = 0;
  int j = 0;
  while (i < 4 && source[i] == 0)
    i++;
  while (j < 4 && dest[j] == 0)
    j++;
  dest[j - 1] = source[i];
  source[i] = 0;
  PrintAll();
  return dest[j - 1];
}

/* Recursive Hanoi: forwards its cell-slice parameters permuted. */
void Hanoi(int n, int *source, int *dest, int *spare) {
  if (n == 1) {
    Move(source, dest);
    return;
  }
  Hanoi(n - 1, source, spare, dest);
  Move(source, dest);
  Hanoi(n - 1, spare, dest, source);
}

int main(void) {
  int i;
  int n;
  int moved;

  for (i = 0; i < 4; i++)
    A[i] = i + 1;
  for (i = 0; i < 4; i++)
    B[i] = 0;
  for (i = 0; i < 4; i++)
    C[i] = 0;

  /* Data-dependent disc count: 4 - 2 + 2 = 4, but only at runtime. */
  n = A[3] - A[1] + 2;
  printf("discs %d\n", n);

  printf("start\n");
  PrintAll();

  /* One move with its return value observed, state printed between. */
  moved = Move(A, C);
  printf("moved %d\n", moved);
  PrintAll();
  moved = Move(C, A);
  printf("back %d\n", moved);

  /* Full solve with permuted recursion. */
  Hanoi(n, A, B, C);

  printf("end\n");
  PrintAll();
  printf("checksum %d\n", A[0] + B[0] + B[3] + C[0]);
  return 0;
}

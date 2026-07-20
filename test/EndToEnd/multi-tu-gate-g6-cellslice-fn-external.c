// REQUIRES: cargo
// W3.3 G6 EndToEnd differential: ONE generic cell-slice function `sum4`
// (`&[Cell<i32>]` parameter) driven from two translation units with two
// DIFFERENT internal-linkage globals — `A` in this TU, `B` in the companion.
// The W3.3 whole-program cell-slice merge promotes the externally visible
// `sum4` even though its bases differ per TU (internal globals never merge
// into a multi-base class), and each call site binds its own global via a
// distinct `emitrust.global_cells` region. Proves the promoted lowering is
// runtime identical to the clang-linked native program.
// RUN: emitrust-cc --emit=crate %s %S/Inputs/multi-tu-gate-g6-cellslice-fn-external-e2e-other.c -o %t.crate --crate-name g6_fn --build
// RUN: clang -std=c11 %s %S/Inputs/multi-tu-gate-g6-cellslice-fn-external-e2e-other.c -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/g6_fn > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

static int A[4];

int sum4(int *a) {
  int s = 0;
  int i;
  for (i = 0; i < 4; i++)
    s = s + a[i];
  return s;
}

// Defined in the companion TU; calls sum4 on its own internal global B.
int use_sum4(void);

int main(void) {
  int i;
  for (i = 0; i < 4; i++)
    A[i] = i + 1;
  int sa = sum4(A);
  int sb = use_sum4();
  printf("sumA=%d sumB=%d\n", sa, sb);
  return 0;
}

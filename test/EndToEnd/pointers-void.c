// REQUIRES: cargo
// CTS-P9 differential end-to-end test: `void *` as a pointee-wildcard
// cursor. An int's address round-trips through a `void *` (and through a
// second `void *` way-station local) and is read and written back
// through `*(int *)p`; a same-width unsigned view reads the stored bit
// pattern and writes a wrapped product back through the view. All values
// are computed across loop iterations, so no access folds to a constant:
// the reinterpreted loads and stores carry genuine runtime data.
// Byte-identical stdout and exit codes against the clang-built native
// binary are required. No UB: every access re-reads the object as a type
// compatible with its effective type (int/unsigned int are compatible
// aliasing-wise).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name pointers_void_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointers_void_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int main(void) {
  int x;
  void *p;
  void *v;
  int *r;
  int i;
  int acc = 0;
  x = 1;
  p = &x;
  for (i = 0; i < 4; i++) {
    *(int *)p = *(int *)p + i;
    acc = acc + *(int *)p;
    printf("x=%d acc=%d\n", x, acc);
  }
  v = p;
  r = (int *)v;
  *r = *r + acc;
  printf("through v: x=%d\n", x);
  {
    unsigned int u;
    u = *(unsigned int *)p;
    u = u * 2654435761u;
    printf("u=%u\n", u);
    *(unsigned int *)p = u;
    printf("x=%d\n", x);
  }
  return 0;
}

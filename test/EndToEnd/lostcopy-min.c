// REQUIRES: cargo
// FR-25: minimal found-in-the-wild reproducer for the lost-copy miscompile
// (SSA-destruction lost-copy problem): a while loop whose back-edge rotates
// three locals (`a = b; b = c; c = a + b;`). The scf.while back-edge rebinds
// all carried values in parallel; emitting the rebinding as sequential Rust
// assignments reads a freshly clobbered carried variable and prints 16
// instead of the correct 13. Differential test: transpile to a cargo crate,
// build it, and compare its stdout against the natively compiled C program.
// The program is deterministic, has no UB, and all values stay small.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/lostcopy_min > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int main(void) {
  int a = 0;
  int b = 1;
  int c = 1;
  while (c < 10) {
    a = b;
    b = c;
    c = a + b;
  }
  printf("%d\n", c);
  return 0;
}

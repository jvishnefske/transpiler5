// REQUIRES: cargo
// FR-26: differential end-to-end test for multi-translation-unit linking. Two
// .c files are transpiled and merged into one crate; an external function
// (add), an external global (shared_counter), and a cross-TU call chain
// (lib_transform -> the library's own static scale) must resolve, while the
// file-static `scale` defined in BOTH translation units must stay distinct.
// The crate build and the clang-linked native binary must produce
// byte-identical stdout. Companion TU lives in Inputs/ (not discovered as a
// test). --release matches the other EndToEnd tests: debug Rust panics on
// overflow where C wraps, and the values here stay small.
// RUN: emitrust-cc --emit=crate %s %S/Inputs/multi-tu-lib.c -o %t.crate --crate-name multi_tu --build
// RUN: clang -std=c11 %s %S/Inputs/multi-tu-lib.c -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/multi_tu > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

// Declarations of symbols defined in the companion translation unit.
int add(int a, int b);
int lib_transform(int x);
extern int shared_counter;

// Internal-linkage helper sharing its spelling with the library's static.
static int scale(int x) { return x + 1; }

int main(void) {
  printf("add=%d\n", add(40, 2));
  printf("shared=%d\n", shared_counter);
  printf("local_scale=%d\n", scale(10));
  printf("lib_transform=%d\n", lib_transform(10));
  int total = 0;
  for (int i = 0; i < 4; ++i) {
    total = total + add(i, scale(i));
  }
  printf("total=%d\n", total);
  return 0;
}

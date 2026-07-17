// REQUIRES: cargo
// CTS-L3 initializers for pointer-typed objects, differential end-to-end
// test: transpile to a cargo crate, build it, and compare its stdout
// against the natively compiled C program. Exercises a file-scope
// `char *` bound to a string literal (walked to its NUL through the
// stored cursor global, then read back through negative subscripts), a
// second literal-bound global initialized at a byte offset, a global
// struct whose fn_ptr fields are initialized with a function reference
// and the null constant, and the 00220 shape: a block-scope wide
// (`wchar_t`, i32) array initialized from a non-ASCII wide literal and
// walked by a `wchar_t *` cursor printing each code unit with %04X. main
// returns 0 and reports everything via printf, so lit's per-command
// exit-code checking covers both runs and diff covers the observable
// behavior. All cursor values stay in bounds; the program has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/globals_string > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stddef.h>

int printf(const char *, ...);

char *s = "hello";
char *t = &"abcd"[2];

int seven(void) { return 7; }
struct Ops {
  int (*get)(void);
  int (*nil)(void);
  int tag;
};
struct Ops ops = {seven, 0, 40};

int walk(void) {
  int n = 0;
  while (*s) {
    n++;
    s++;
  }
  return n;
}

int main(void) {
  printf("%d\n", walk());
  printf("%c%c\n", s[-5], s[-1]);
  printf("%c%c\n", t[0], t[1]);
  printf("%d %d\n", ops.get(), ops.tag);
  wchar_t w[] = L"h€猫z";
  wchar_t *p;
  for (p = w; *p; p++)
    printf("%04X ", (unsigned)*p);
  printf("\n");
  return 0;
}

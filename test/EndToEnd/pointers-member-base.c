// REQUIRES: cargo
// CTS-P9 differential end-to-end test: `&struct.member` pointer bases
// (the 00163 shape). One pointer rebinds between a local scalar and a
// global struct's member at a data-dependent loop iteration, so every
// dereference dispatches on a genuine runtime discriminant; a second
// pointer roots at a local struct's member. Writes through the pointers
// must be visible through the structs themselves and vice versa.
// Byte-identical stdout and exit codes against the clang-built native
// binary are required. No UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name pointers_member_base_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointers_member_base_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct ziggy {
  int a;
  int b;
  int c;
} bol;

int main(void) {
  int a;
  int *b;
  int i;
  a = 40;
  bol.a = 12;
  bol.b = 7;
  bol.c = 56;
  b = &a;
  for (i = 0; i < 4; i++) {
    *b = *b + i;
    printf("i=%d a=%d bol.b=%d\n", i, a, bol.b);
    if (i == 1)
      b = &bol.b;
  }
  printf("bol: %d %d %d\n", bol.a, bol.b, bol.c);
  *b = *b + a;
  printf("after: a=%d bol.b=%d *b=%d\n", a, bol.b, *b);
  {
    struct ziggy loc;
    int *m;
    loc.a = a;
    loc.b = 2;
    loc.c = 3;
    m = &loc.b;
    *m = *m + loc.a;
    printf("loc: %d %d %d *m=%d\n", loc.a, loc.b, loc.c, *m);
  }
  return 0;
}

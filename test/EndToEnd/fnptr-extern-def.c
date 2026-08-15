// REQUIRES: cargo
// FR-77 acceptance half: a file-scope function-pointer initializer taking the
// address of a function DEFINED IN A SIBLING TU (the reduced tinycrypt ecc.c
// shape -- `static RNG g_rng = &default_csprng;` with default_csprng living in
// its own TU) must keep transpiling, and the built crate's stdout must be
// byte-identical to the clang-linked native binary. This is the differential
// guard on the FR-77 rejection: the new undefined-target refusal fires ONLY
// when no TU defines the name, never on the cross-TU-defined case. The seed
// derives from argc so the buffer length is opaque to both compilers and
// constant folding cannot hide a miscompile; every indirect call is
// null-checked, the program has no UB.
// RUN: emitrust-cc --emit=crate %s %S/Inputs/fnptr-extern-def-lib.c \
// RUN:   -o %t.crate --crate-name fnptr_extern_def --build
// RUN: clang -std=c11 %s %S/Inputs/fnptr-extern-def-lib.c -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/fnptr_extern_def > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

typedef int (*RNG)(unsigned char *dest, unsigned int size);
extern int default_csprng(unsigned char *dest, unsigned int size);

/* The file-scope address-of that FR-77 polices: the initializer is the only
 * reference to default_csprng in THIS TU. */
static RNG g_rng = &default_csprng;

void set_rng(RNG f) { g_rng = f; }
RNG get_rng(void) { return g_rng; }

int gen(unsigned char *buf, unsigned int n) {
  RNG f = g_rng;
  return f ? f(buf, n) : 0;
}

int main(int argc, char **argv) {
  unsigned char buf[8];
  unsigned int n = (unsigned int)argc * 4u; /* 4 when run plain; opaque */
  unsigned int i;
  int ok = gen(buf, n);
  printf("ok=%d\n", ok);
  for (i = 0; i < n; ++i)
    printf("%u\n", (unsigned)buf[i]);
  set_rng(0);
  printf("cleared=%d\n", gen(buf, n));
  printf("null=%d\n", get_rng() == 0);
  return 0;
}

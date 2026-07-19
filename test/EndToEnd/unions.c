// REQUIRES: cargo
// B1 differential end-to-end test: named unions modeled as one-field
// structs. Exercises the int/unsigned pun in both directions (store a
// negative int, print the unsigned arm; store a large unsigned, print the
// int arm), the FLOAT puns (float/u32 and double/i64 — store a float,
// print the bits; store bits, print the float — the classic type-punning
// shapes, mapped to Rust's to_bits/from_bits), a union global with a
// constant initializer read through the other arm, a float-pun global
// whose designated integer-arm initializer crosses the domain at compile
// time, designated LOCAL initializers through pun arms, compound
// assignment and ++ through pun arms, a value-position assignment read
// through the assigned pun arm, a union nested in a struct, and a
// single-arm union. Every observable value is printed and diffed against
// the native build.
// No UB: reading a union member after storing another is defined (C11
// 6.5.2.3 with footnote 95 — the bytes are reinterpreted); int/unsigned
// share width on this target, so that pun is bit-exact two's complement
// in both languages; and the float puns store/read exact bit patterns of
// exactly representable values, so no rounding is observable. --release
// is load-bearing: debug Rust panics on integer overflow where C wraps,
// and the compound-assignment probe deliberately wraps unsigned
// 0xffffffff + 1 to 0 (defined in C, wrapping in release Rust).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/unions > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

union Pun {
  int i;
  unsigned int u;
};

union FPun {
  float f;
  unsigned int u;
};

union DPun {
  double d;
  long b;
};

union FPun gf = {.u = 0x3fc00000u}; /* the bits of 1.5f */

union One {
  int only;
};

struct Holder {
  int tag;
  union Pun p;
};

union Pun g = {-7};

int main(void) {
  union Pun v;
  v.i = -1;
  printf("pun=%u\n", v.u);
  v.u = 3000000000u;
  printf("back=%d\n", v.i);

  printf("g=%d,%u\n", g.i, g.u);

  struct Holder h;
  h.tag = 3;
  h.p.i = -100;
  printf("holder=%d,%d,%u\n", h.tag, h.p.i, h.p.u);
  h.p.u = 2147483648u;
  printf("holder2=%d\n", h.p.i);

  union One s;
  s.only = 42;
  printf("one=%d\n", s.only);

  /* Float puns: exact powers-of-two shapes, bit patterns printed as
     integers so the comparison is representation-exact. */
  union FPun f;
  f.f = 1.5f;
  printf("fbits=%u\n", f.u);
  f.u = 0x40200000u; /* the bits of 2.5f */
  printf("fval=%f\n", (double)f.f);
  printf("gf=%f,%u\n", (double)gf.f, gf.u);

  union DPun d;
  d.d = -2.0;
  printf("dbits=%d\n", d.b < 0);
  d.b = 0x4010000000000000L; /* the bits of 4.0 */
  printf("dval=%f\n", d.d);

  /* Designated local initializers through pun arms (both directions). */
  union Pun pi = {.u = 4294967295u};
  printf("pinit=%d\n", pi.i);
  union FPun fi = {.u = 0x3f000000u}; /* the bits of 0.5f */
  printf("finit=%f\n", (double)fi.f);

  /* Compound assignment and ++ through pun arms: the loaded slot value
     reinterprets to the arm, computes there, and reinterprets back. */
  f.f = 2.0f;
  f.u += 1u;
  printf("fca=%u\n", f.u);
  f.u++;
  printf("fpp=%u\n", f.u);
  pi.u += 1u; /* 0xffffffff + 1 wraps to 0 in both languages */
  printf("pca=%d\n", pi.i);
  pi.i--;
  printf("pmm=%u\n", pi.u);

  /* Value-position assignment through a pun arm: the re-loaded slot
     value reinterprets to the assigned arm's own type. */
  unsigned vp = (pi.u = 7u);
  printf("vp=%u\n", vp);
  return 0;
}

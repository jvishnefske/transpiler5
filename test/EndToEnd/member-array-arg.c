// REQUIRES: cargo
// FR-74: differential end-to-end test for MEMBER-ARRAY arguments to
// slice parameters — tinycrypt's `compress(s->iv, s->leftover)` shape.
// `compress` passes its struct-pointer parameter's member arrays through
// an ARROW base (`s->a`), including the disjoint-sibling-fields call
// `mix(s->a, s->b, s->n)` — two member arrays of ONE struct into two
// mutable slice parameters at once, provably non-overlapping in C and a
// legal two-&mut borrow shape in Rust — and main passes a DOT base
// (`s.b`) over a local struct. The admitted lowering (slice_of over the
// member place at cursor 0) must produce the SAME observable behavior
// as clang compiling the original natively: every buffer byte is
// printed and all stored values derive from argc so constant folding
// cannot pre-compute the transforms and hide a miscompile. `cargo
// build` success alone proves nothing here — the stdout diff is the
// oracle. Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/member_array_arg > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/member_array_arg a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>

struct S {
  unsigned char a[8];
  unsigned char b[8];
  unsigned int n;
};

static void bump(unsigned char *buf, unsigned len) {
  unsigned i;
  for (i = 0; i < len; i++)
    buf[i] = (unsigned char)(buf[i] + 1u);
}

static void mix(unsigned char *x, unsigned char *y, unsigned len) {
  unsigned i;
  for (i = 0; i < len; i++) {
    x[i] = (unsigned char)(x[i] + y[i]);
    y[i] = (unsigned char)(y[i] ^ x[i]);
  }
}

static void compress(struct S *s) {
  bump(s->a, s->n);
  mix(s->a, s->b, s->n);
}

int main(int argc, char **argv) {
  struct S s;
  unsigned i;
  s.n = 8u;
  for (i = 0; i < 8u; i++) {
    s.a[i] = (unsigned char)(i + (unsigned)argc);
    s.b[i] = (unsigned char)(i * 3u + (unsigned)argc);
  }
  compress(&s);
  bump(s.b, s.n);
  for (i = 0; i < 8u; i++)
    printf("%u %u\n", (unsigned)s.a[i], (unsigned)s.b[i]);
  return 0;
}

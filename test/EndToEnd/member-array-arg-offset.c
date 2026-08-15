// REQUIRES: cargo
// FR-86: differential end-to-end test for OFFSET member-array arguments
// to slice parameters — tinycrypt's `add_round_key(state, s->words +
// Nb*Nr)` and hmac's `&dummy_state.key[TC_SHA256_DIGEST_SIZE]` shapes.
// Every admitted offset spelling appears: dot-root `s.key + 4`
// (constant), arrow-root `sp->key + 2` (constant), arrow-root
// `&sp->key[off]` (runtime pure), dot-root `&s.key[off]` (runtime pure,
// shared read), and the top-level `&local[off]` reslice next to them —
// with MUTATION through the offset slices so a cursor that lands one
// element off rewrites the wrong bytes and the diff catches it. All
// seeds and the runtime offset derive from argc so constant folding
// cannot pre-compute the buffers and hide a miscompile; every buffer
// byte is printed. `cargo build` success alone proves nothing here —
// the stdout diff is the oracle. Deterministic, no UB (off = argc + 2
// stays in bounds for both runs below).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/member_array_arg_offset > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/member_array_arg_offset a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>

struct S {
  unsigned char key[16];
  unsigned int n;
};

static void fill(unsigned char *p, unsigned int n, unsigned char seed) {
  unsigned int i;
  for (i = 0u; i < n; i++)
    p[i] = (unsigned char)(seed + i);
}

static unsigned int sum(const unsigned char *p, unsigned int n) {
  unsigned int i;
  unsigned int t = 0u;
  for (i = 0u; i < n; i++)
    t += (unsigned int)p[i];
  return t;
}

static void dump(const unsigned char *p, unsigned int n) {
  unsigned int i;
  for (i = 0u; i < n; i++)
    printf("%u\n", (unsigned int)p[i]);
}

static void touch(struct S *sp, unsigned int off) {
  fill(sp->key + 2, 4u, 55u);
  fill(&sp->key[off], 3u, 77u);
}

int main(int argc, char **argv) {
  struct S s;
  unsigned char local[24];
  unsigned int off = (unsigned int)argc + 2u;
  fill(s.key, 16u, (unsigned char)argc);
  fill(local, 24u, 100u);
  fill(s.key + 4, 8u, (unsigned char)(argc + 7));
  fill(&local[off], 5u, 33u);
  touch(&s, off);
  printf("%u\n", sum(&s.key[off], 6u));
  dump(s.key, 16u);
  dump(local, 24u);
  return 0;
}

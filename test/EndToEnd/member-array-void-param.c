// REQUIRES: cargo
// FR-89: differential end-to-end test for MEMBER-ARRAY arguments to a
// void*-ADMITTED byte-cursor parameter — tinycrypt's
// `_set(prng->key, 0x00, sizeof(prng->key))` shape (hmac_prng.c:143),
// where the argument reaches the FR-71 param through the implicit
// void* BitCast Sema inserts, one AST node the FR-74/86 typed-path
// pins never see. Both the whole-member (`s.iv`) and the
// constant-offset (`s.iv + 4`) spellings pass through the SAME void*
// callee with MUTATION through the region, the offset region is
// visibly distinct from the surrounding bytes (a cursor off by one
// rewrites the wrong bytes and the diff catches it), and every buffer
// byte plus the struct's scalar sibling is printed. All stored values
// derive from argc so constant folding cannot pre-compute the buffer
// and hide a miscompile; the second RUN pair re-seeds via extra argv
// words. `cargo build` success alone proves nothing here — the stdout
// diff against the clang-built native is the oracle. Deterministic,
// no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/member_array_void_param > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/member_array_void_param a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>
typedef unsigned char u8;
struct S { u8 iv[16]; unsigned n; };
static void setb(void *to, u8 val, unsigned len) {
  u8 *d = (u8 *)to;
  unsigned i;
  for (i = 0; i < len; i++) d[i] = val;
}
int main(int argc, char **argv) {
  struct S s;
  unsigned i;
  s.n = (unsigned)argc;
  setb(s.iv, (u8)(argc + 6), 16u);
  setb(s.iv + 4, (u8)(argc * 3), 4u);
  for (i = 0; i < 16u; i++) printf("%u ", (unsigned)s.iv[i]);
  printf("| %u\n", s.n);
  return 0;
}

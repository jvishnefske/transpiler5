// REQUIRES: cargo
// FR-72: differential end-to-end test for byte-family <string.h> calls
// over byte-slice PARAMETERS. `_set` is tinycrypt's literal memset-alike
// — a `void *` parameter whose ONLY use is the memset destination (the
// shape FR-71's cast-walk could never admit; the byte-family-call arm
// does) — and `_copy`/`_cmp` are its `uint8_t *`/`const uint8_t *`
// siblings; `_fill` writes through a NONZERO cursor (`dst + 1`) so the
// reslice offset itself is under test, and main also passes a nonzero
// call-site offset (`b + 1`). The admitted parameters must produce the
// SAME observable behavior as clang compiling the original natively:
// every buffer byte is printed and all stored values derive from argc so
// constant folding cannot pre-compute the fills and hide a miscompile.
// `cargo build` success alone proves nothing here — the stdout diff is
// the oracle. memcmp results are consumed ONLY through ==0/>0/<0: C
// defines only the sign, and glibc's return magnitude differs from the
// helper's byte difference, so printing the raw value would diff for a
// reason that is not a miscompile. The functions are non-static on
// purpose: this test pins the byte-family-call admission for
// EXTERNAL-linkage helpers; the file-static leading-underscore form now
// folds its tu-prefix boundary (`tu0_set`, FR-73) and is pinned by
// static-underscore-helper.c. Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/string_fn_slice_param > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/string_fn_slice_param a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>
#include <string.h>
typedef unsigned char uint8_t;

void _set(void *to, uint8_t val, unsigned len) {
  memset(to, val, len);
}

void _fill(uint8_t *dst, int val, unsigned len) {
  memset(dst + 1, val, len);
}

void _copy(uint8_t *dst, const uint8_t *src, unsigned len) {
  memcpy(dst, src, len);
}

int _cmp(const uint8_t *a, const uint8_t *b, unsigned len) {
  return memcmp(a, b, len);
}

int main(int argc, char **argv) {
  uint8_t a[8];
  uint8_t b[8];
  unsigned i;

  _set(a, (uint8_t)(argc + 64), 8u);
  _fill(a, argc + 6, 4u);
  _set(b, (uint8_t)argc, 8u);
  _copy(b, a, 8u);
  for (i = 0; i < 8u; i++)
    printf("%u ", (unsigned)b[i]);
  printf("\n");
  // A nonzero CALL-SITE offset: the reslice inside _copy starts where
  // the argument's cursor says, not at the parameter's origin.
  _copy(b + 1, a, 4u);
  for (i = 0; i < 8u; i++)
    printf("%u ", (unsigned)b[i]);
  printf("\n");
  // After the copies b differs from a at index 1 (b[1] holds a[0], the
  // fill base, ABOVE a's low fill byte a[1]) while b+1 opens with a's
  // first four bytes, so all three sign classes are exercised — less,
  // greater, and equal — and each prints 1 only when the helper's sign
  // agrees with glibc's.
  printf("%d %d %d\n", _cmp(a, b, 8u) < 0, _cmp(b, a, 8u) > 0,
         _cmp(a, b + 1, 4u) == 0);
  _set(b, (uint8_t)(argc + 1), 8u);
  printf("%d\n", _cmp(a, b, 8u) == 0);
  return 0;
}

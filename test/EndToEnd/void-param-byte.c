// REQUIRES: cargo
// FR-71: differential end-to-end test for `void *` parameters admitted as
// byte cursors. `_set` is tinycrypt's memset-alike (`void *` walked
// through a `uint8_t *` local) and `_compare` its constant-time compare
// (`const void *` pair read through `const uint8_t *` locals) — the
// top-ranked void*-parameter diagnostic family of the 2026-08-14 Track 5
// re-measurement. The admitted params must produce the SAME observable
// behavior as clang compiling the void* original natively: fills, an
// equal and an unequal compare, and every buffer byte are printed, with
// all stored values derived from argc so constant folding cannot
// pre-compute the digests and hide a miscompile. `cargo build` success
// alone proves nothing here — the stdout diff is the oracle. The
// functions are non-static on purpose: a static leading-underscore name
// emits the tu-prefixed `tu0__set`, which trips the crate's denied
// non_snake_case lint — a pre-existing, param-type-independent naming
// defect outside FR-71's scope. main returns 0 and reports everything
// through printf, so lit's per-command exit-code checking covers both
// runs and diff covers the observable behavior. Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/void_param_byte > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

typedef unsigned char uint8_t;
typedef unsigned long size_t;

extern int printf(const char *, ...);

void _set(void *to, uint8_t val, unsigned len) {
  uint8_t *t = to;
  while (len--) {
    *t++ = val;
  }
}

int _compare(const void *a, const void *b, size_t size) {
  const uint8_t *tempa = a;
  const uint8_t *tempb = b;
  uint8_t result = 0;
  for (size_t i = 0; i < size; i++)
    result |= tempa[i] ^ tempb[i];
  return result;
}

int main(int argc, char **argv) {
  uint8_t buf[8];
  uint8_t ref[8];
  unsigned i;

  _set(buf, (uint8_t)argc, 8u);
  _set(ref, (uint8_t)argc, 8u);
  printf("eq=%d\n", _compare(buf, ref, 8));
  ref[3] = (uint8_t)(argc + 5);
  printf("ne=%d\n", _compare(buf, ref, 8));
  _set(buf, (uint8_t)(argc * 3 + 1), 4u);
  for (i = 0; i < 8u; i++)
    printf("%u ", (unsigned)buf[i]);
  printf("\n");
  printf("final=%d\n", _compare(buf, ref, 8));
  return 0;
}

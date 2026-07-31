// REQUIRES: cargo
// FR-55: differential end-to-end test for the borrow shape a `const
// unsigned char *` parameter gets. `digest` walks its const byte pointer
// (a shared `&[u8]`) and hands ONE element of it to `mix`, whose own const
// byte pointer is only dereferenced — the exact shape of every
// `set_key(ctx, const uint8_t *k)`/`encrypt_block(..., const uint8_t *in)`
// wrapper in real crypto code. Before FR-55 the deref-only parameter came
// out `&mut u8` while the walked one came out `&[u8]`, so the element
// borrow was a mutable reborrow of a shared reference and the emitted
// crate did not compile (`error[E0596]`). The buffers are 40 bytes on
// purpose: past the owner-promotion ceiling, so the parameters really are
// slices rather than a promoted owner receiver. The address-of form of the
// same argument (`mix(&single)`) is covered too.
// main returns 0 and reports everything through printf, so lit's
// per-command exit-code checking covers both runs and diff covers the
// observable behavior. The program is deterministic and has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/const_byte_arg > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

static unsigned char mix(const unsigned char *p) {
  return (unsigned char)(*p * 3 + 1);
}

static int digest(const unsigned char *k, int n, unsigned char *out) {
  int i;
  for (i = 0; i < n; i++)
    out[i] = mix(&k[i]);
  return n;
}

int main(void) {
  unsigned char key[40];
  unsigned char out[40];
  unsigned char single = 200;
  int i;
  int sum = 0;

  for (i = 0; i < 40; i++) {
    key[i] = (unsigned char)(i * 5 + 1);
    out[i] = 0;
  }
  printf("n=%d\n", digest(key, 40, out));
  for (i = 0; i < 40; i++)
    sum += (int)out[i];
  printf("sum=%d first=%d last=%d\n", sum, (int)out[0], (int)out[39]);
  printf("single=%d\n", (int)mix(&single));
  return 0;
}

// REQUIRES: cargo
// FR-55: differential end-to-end test for struct fields BEYOND the reach of
// Rust's `#[derive(Default)]`. The standard library's blanket
// `impl<T: Default> Default for [T; N]` stops at N = 32, so every one of
// these structs (a crypto state struct's exact shape: `[u32; 8]` IV,
// `[u8; 64]` block buffer, `[u32; 68]` round-key schedule) needs the
// explicit `impl Default` the emitter writes in the derive's place. The
// point of the test is that the explicit impl produces the SAME values the
// derive would have: the program reads every byte of a zero-initialized
// object back, then overwrites and re-reads it, and its stdout is compared
// against the natively compiled C. Nesting is covered both ways round — a
// struct holding an oversized struct (`one`), and an array of oversized
// structs (`slots`) — as is a plain struct copy of the whole thing.
// main returns 0 and reports everything through printf, so lit's
// per-command exit-code checking covers both runs and diff covers the
// observable behavior. The program is deterministic and has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/struct_long_arrays > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct inner {
  unsigned char tail[48];
  int n;
};

struct state {
  unsigned int iv[8];
  unsigned char buf[64];
  unsigned int rk[68];
  struct inner one;
  struct inner slots[3];
  double d;
};

int main(void) {
  struct state s = {0};
  int i;
  int j;
  int nonzero = 0;
  int sum = 0;

  for (i = 0; i < 8; i++)
    if (s.iv[i] != 0u)
      nonzero++;
  for (i = 0; i < 64; i++)
    if (s.buf[i] != 0)
      nonzero++;
  for (i = 0; i < 68; i++)
    if (s.rk[i] != 0u)
      nonzero++;
  for (i = 0; i < 48; i++)
    if (s.one.tail[i] != 0)
      nonzero++;
  if (s.one.n != 0)
    nonzero++;
  for (i = 0; i < 3; i++) {
    for (j = 0; j < 48; j++)
      if (s.slots[i].tail[j] != 0)
        nonzero++;
    if (s.slots[i].n != 0)
      nonzero++;
  }
  if (s.d != 0.0)
    nonzero++;
  printf("default nonzero=%d\n", nonzero);

  for (i = 0; i < 8; i++)
    s.iv[i] = (unsigned int)(i * 7 + 1);
  for (i = 0; i < 64; i++)
    s.buf[i] = (unsigned char)(i * 3 + 5);
  for (i = 0; i < 68; i++)
    s.rk[i] = (unsigned int)(i * 11 + 2);
  for (i = 0; i < 48; i++)
    s.one.tail[i] = (unsigned char)(i + 9);
  s.one.n = -12345;
  for (i = 0; i < 3; i++) {
    for (j = 0; j < 48; j++)
      s.slots[i].tail[j] = (unsigned char)(i * 48 + j);
    s.slots[i].n = i - 1;
  }

  for (i = 0; i < 8; i++)
    sum += (int)s.iv[i];
  for (i = 0; i < 64; i++)
    sum += (int)s.buf[i];
  for (i = 0; i < 68; i++)
    sum += (int)s.rk[i];
  for (i = 0; i < 48; i++)
    sum += (int)s.one.tail[i];
  sum += s.one.n;
  for (i = 0; i < 3; i++) {
    for (j = 0; j < 48; j++)
      sum += (int)s.slots[i].tail[j];
    sum += s.slots[i].n;
  }
  printf("written sum=%d\n", sum);

  struct state t;
  t = s;
  t.buf[0] = 200;
  printf("copy=%d orig=%d rk67=%u slot2tail47=%d\n", (int)t.buf[0],
         (int)s.buf[0], t.rk[67], (int)t.slots[2].tail[47]);
  return 0;
}

// REQUIRES: cargo
// CTS-P11 (00217 byte puns), differential end-to-end test: 4-byte
// reads, writes, and compound assignments through `*(unsigned *)` views
// over char arrays at RUNTIME offsets, lowered via
// `u32::from_ne_bytes` / `.to_ne_bytes()` over the byte run. The risky
// seams driven here: the 00217 shape itself (a char* into a global
// char array, compound-assigned through the wide view with a wrapping
// u32 delta); byte coherence (a %s print of the same buffer must see
// every byte the wide view wrote, and a wide read must see bytes
// written per-element); read-modify-write compounds at offsets that
// only overlap partially with an earlier wide store; and all mutated
// bytes staying printable ASCII so the %s differential is exact.
// Byte-identical stdout and exit codes against the natively compiled
// program are required; the pun must go through ne_bytes with no
// unsafe.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name byte_puns --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/byte_puns > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: grep -F "u32::from_ne_bytes" %t.crate/src/main.rs
// RUN: grep -F "to_ne_bytes" %t.crate/src/main.rs
// RUN: not grep "unsafe" %t.crate/src/main.rs

int printf(const char *, ...);

char gt[10];

int main(void) {
  char buf[12];
  int i;
  unsigned long long r;
  unsigned a;
  unsigned long long b;
  unsigned u;
  unsigned g;
  char *data;

  /* ---- local char array ---- */
  for (i = 0; i < 11; i++)
    buf[i] = (char)('A' + i);
  buf[11] = 0;
  printf("init  \"%s\"\n", buf);

  /* Runtime offset and a wrapping compound delta (the 00217 shape:
     a - b is 5 - 12 = -7 mod 2^32). Only the low byte changes:
     'E' -> '>'. */
  r = 4;
  a = 5;
  b = 12;
  *(unsigned *)(buf + r) += a - b;
  printf("comp  \"%s\"\n", buf);

  /* Wide read back through the same view: sees the compound's bytes. */
  u = *(unsigned *)(buf + r);
  printf("u=%u\n", u);

  /* Wide store at a partially overlapping runtime offset. */
  *(unsigned *)(buf + (r - 2)) = u;
  printf("store \"%s\"\n", buf);

  /* Per-element write, then a wide read must see it. */
  buf[3] = 'z';
  u = *(unsigned *)(buf + 2);
  printf("mix u=%u \"%s\"\n", u, buf);

  /* ---- global char array through a char* local (00217 exactly) ---- */
  for (i = 0; i < 9; i++)
    gt[i] = (char)('a' + i);
  gt[9] = 0;
  data = gt;
  *(unsigned *)(data + (r - 1)) += 2;
  printf("gcomp \"%s\"\n", gt);
  printf("gdata \"%s\"\n", data);

  g = *(unsigned *)(gt + r);
  printf("g=%u\n", g);

  *(unsigned *)(data + 1) = g;
  printf("gstore \"%s\"\n", gt);

  return 0;
}

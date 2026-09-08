// REQUIRES: cargo
// FR-193 item 2: a `%s` directive carrying a FIELD WIDTH was the one `%s`
// shape FR-191 left on the Latin-1 `__emitrust_cstr` Display funnel, on the
// reasoning that "a raw `write_all` cannot pad". The reasoning was true and
// the consequence was a LIVE MISCOMPILE, not a scope note: every payload byte
// >= 0x80 came out as TWO UTF-8 bytes. Measured on the baseline tool for
// `printf("A[%10s]\n", buf)` with buf = 81 8e 9b a8 7a:
//     native : 41 5b 20 20 20 20 20 81 8e 9b a8 7a 5d 0a
//     emitted: 41 5b 20 20 20 20 20 c2 81 c2 8e c2 9b c2 a8 7a 5d 0a
// Note what is NOT wrong there: the PAD COUNT is right in both (five spaces).
// `__emitrust_cstr` yields exactly one Rust `char` per C byte and Rust's
// `{:>10}` pads by char count, so the width arithmetic already agreed with C;
// only the ENCODING of the payload diverged. This test pins the repaired
// behaviour -- raw bytes plus raw padding computed from the C byte length --
// across every width/precision combination, in both alignments, with the
// field wider than, equal to, and narrower than the payload, and with an
// empty payload where the output is pure padding.
//
// Pure-ASCII twins ride along for every non-ASCII shape. They were already
// correct before the fix, so they pin the other half of the contract: the
// repair must not shift a single byte on the ASCII path.
//
// Two shapes deliberately stay on the formatter and are pinned here as
// still-correct rather than silently dropped: a string LITERAL argument
// (already a `&'static str`, never in the byte funnel, and ASCII by import
// rule) and a `%s` whose LATER argument has side effects (FR-191's ordering
// fence -- C evaluates every argument before printf writes anything, so a raw
// write that flushes a segment mid-scan would invert the output order). The
// second is exercised with an ASCII payload only, because the declined bypass
// leaves it on the Latin-1 funnel; that residual is FR-194's pinned one.
//
// Every payload byte is derived from `argc` and the test runs under three
// different argument vectors, so neither clang nor rustc can constant-fold
// the buffer and hide a miscompile behind a folded literal. A FileCheck of
// the IR cannot catch this class -- only a byte diff can, which is why the
// payload also makes a round trip through real file I/O.
// The scratch filename is unique to this test because lit runs every EndToEnd
// binary in a SHARED per-directory cwd (build/test/EndToEnd).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name printf_string_width_nonascii_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/printf_string_width_nonascii_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native.out
// RUN: %t.crate/target/release/printf_string_width_nonascii_e2e a b > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b c d e > %t.native.out
// RUN: %t.crate/target/release/printf_string_width_nonascii_e2e a b c d e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>

/* A file-scope char buffer: the staged-global-copy `%s` shape. */
char gwbuf[8];

static int seq;

/* A LATER printf argument that writes stdout itself: the ordering fence. */
static int noisy(void) {
  seq = seq + 1;
  printf("<%d>", seq);
  return seq;
}

int main(int argc, char **argv) {
  char buf[16];
  char empty[4];
  char ascii[16];
  char line[32];
  const char *lit = "abcdef";
  FILE *f;
  int i;

  /* 0x81..0xc6: always non-ASCII, never NUL, and argc-dependent. */
  for (i = 0; i < 4; i++)
    buf[i] = (char)(0x80 + i * 13 + argc);
  buf[4] = 'z';
  buf[5] = '\0';
  for (i = 0; i < 3; i++)
    gwbuf[i] = (char)(0xf0 + i + argc);
  gwbuf[3] = '\0';
  empty[0] = '\0';
  for (i = 0; i < 5; i++)
    ascii[i] = (char)('a' + i);
  ascii[5] = '\0';

  /* Field WIDER than the payload, both alignments. */
  printf("A[%10s]\n", buf);
  printf("B[%-10s]\n", buf);
  printf("a[%10s]\n", ascii);
  printf("b[%-10s]\n", ascii);

  /* Field EQUAL to the payload byte length (5): no padding at all. */
  printf("C[%5s]\n", buf);
  printf("D[%-5s]\n", buf);
  printf("c[%5s]\n", ascii);
  printf("d[%-5s]\n", ascii);

  /* Field NARROWER than the payload: C never truncates on width alone. */
  printf("E[%3s]\n", buf);
  printf("F[%-3s]\n", buf);
  printf("e[%3s]\n", ascii);
  printf("f[%-3s]\n", ascii);

  /* Precision alone (FR-191 already handles this: regression guard). */
  printf("G[%.3s]\n", buf);
  printf("g[%.3s]\n", ascii);

  /* Width AND precision: the pad is computed from the TRUNCATED byte
     length, so the two interact. */
  printf("H[%10.3s]\n", buf);
  printf("I[%-10.3s]\n", buf);
  printf("J[%2.3s]\n", buf);
  printf("h[%10.3s]\n", ascii);
  printf("i[%-10.3s]\n", ascii);
  printf("j[%2.3s]\n", ascii);

  /* Precision ZERO under a width: pure padding, no payload byte. */
  printf("K[%6.0s]\n", buf);
  printf("k[%6.0s]\n", ascii);

  /* An EMPTY payload under a width: pure padding again, this time because
     the NUL is at offset 0 rather than because the precision bounds it. */
  printf("L[%4s]\n", empty);
  printf("M[%-4s]\n", empty);

  /* A mid-buffer `&buf[i]` and a staged global copy, both padded. */
  printf("N[%8s]\n", &buf[2]);
  printf("O[%-8s]\n", &buf[2]);
  printf("P[%8s]\n", gwbuf);
  printf("Q[%-8s]\n", gwbuf);

  /* Format text on BOTH sides of a padded hole plus a trailing `%d`: the
     padded raw write has to land between the two `print!` segments. */
  printf("pre[%8s]post %d\n", buf, argc);
  printf("pre[%-8s]post %d\n", buf, argc);

  /* Two padded holes in ONE call: two flushes, in order. */
  printf("R[%7s][%-7s]\n", buf, gwbuf);

  /* A string LITERAL argument keeps the formatter path (ASCII by import
     rule) -- pinned as still correct, not silently dropped. */
  printf("S[%10s][%-10s][%10.3s]\n", "abc", "abc", "abcdef");
  printf("T[%9s]\n", lit + 2);

  /* FR-191's ordering fence: a LATER argument with side effects declines
     the raw bypass, so this padded `%s` stays on the Latin-1 funnel. ASCII
     payload only -- the non-ASCII residual there is FR-194's, still open. */
  printf("U[%8s] %d\n", ascii, noisy());
  printf("V[%-8s] %d\n", ascii, noisy());

  /* The same bytes through real file I/O, so they cannot be folded. */
  f = fopen("printf_string_width_nonascii_scratch.bin", "w");
  if (!f) {
    printf("open for write failed\n");
    return 1;
  }
  fwrite(buf, 1, 5, f);
  fwrite("\n", 1, 1, f);
  fclose(f);

  f = fopen("printf_string_width_nonascii_scratch.bin", "r");
  if (!f) {
    printf("open for read failed\n");
    return 1;
  }
  if (!fgets(line, 32, f)) {
    printf("read failed\n");
    return 1;
  }
  fclose(f);
  printf("W[%12s]", line);
  printf("X[%-12s]", line);
  printf("Y[%12.4s]\n", line);
  return 0;
}

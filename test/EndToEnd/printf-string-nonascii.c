// REQUIRES: cargo
// FR-191: the byte-level differential this defect class had NO test for.
// Every `%s` payload in the rest of the EndToEnd suite is ASCII, so the
// `__emitrust_cstr` Latin-1 `Display` funnel — which renders each byte as a
// Rust `char` and therefore re-encodes any byte >= 0x80 as TWO UTF-8 bytes —
// looked correct in every existing diff. This test pins that a `char` buffer
// holding high bytes prints BYTE FOR BYTE like the clang-built native binary
// through every `%s` shape the importer admits: a whole local array, a
// mid-buffer `&buf[i]`, a `%.Ns` precision, a global array, a pointer into a
// string literal, a `%s` hole with format text and a later `%d` hole around
// it (so the raw-byte write must be sequenced between two `print!` segments),
// `puts`, and a buffer whose bytes come back from real file I/O (`fgets`), so
// the payload cannot be folded at compile time in either toolchain. Two ASCII
// payloads ride along to pin the shapes that deliberately KEEP the formatter
// path: a field width (`%10s`/`%-10s`, which a raw write could not pad) and a
// call whose LATER argument writes stdout itself (C evaluates every argument
// before printf emits anything, so the raw bypass must be declined there --
// this pins the resulting order as observable bytes).
// Every payload byte is additionally derived from `argc`, and the test runs
// with three different argument vectors, so neither clang nor rustc can
// constant-fold the buffer and hide a miscompile behind a folded literal.
// A FileCheck of the IR cannot catch this class -- only the diff can.
// The scratch filename is unique to this test because lit runs every EndToEnd
// binary in a SHARED per-directory cwd (build/test/EndToEnd).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name printf_string_nonascii_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/printf_string_nonascii_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native.out
// RUN: %t.crate/target/release/printf_string_nonascii_e2e a b > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b c d e > %t.native.out
// RUN: %t.crate/target/release/printf_string_nonascii_e2e a b c d e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>

/* A file-scope char buffer: the staged-global-copy `%s` shape. */
char gbuf[8];

static int seq;

/* A LATER printf argument that writes stdout itself. C evaluates every
   argument before printf emits anything, so this call's own output must
   land AFTER "<1>". Flushing a format segment mid-directive-scan would
   invert that, which is why the raw-bytes bypass is declined when a later
   argument has side effects -- pinned here as observable BYTES. */
static int noisy(void) {
  seq = seq + 1;
  printf("<%d>", seq);
  return seq;
}

int main(int argc, char **argv) {
  char buf[16];
  char line[32];
  const char *lit = "abc";
  FILE *f;
  int i;

  /* 0x81..0xc6: always non-ASCII, never NUL, and argc-dependent. */
  for (i = 0; i < 6; i++)
    buf[i] = (char)(0x80 + i * 13 + argc);
  buf[6] = 'z';
  buf[7] = '\0';
  for (i = 0; i < 4; i++)
    gbuf[i] = (char)(0xf0 + i + argc);
  gbuf[4] = '\0';

  printf("arr[%s]\n", buf);
  printf("mid[%s]\n", &buf[3]);
  printf("prec[%.3s]\n", buf);
  printf("glob[%s]\n", gbuf);
  printf("lit[%s]\n", lit + 1);
  /* Format text on BOTH sides of the hole plus a trailing `%d`: the raw
     byte write has to land between the two `print!` segments, in order. */
  printf("pre[%s]post %d\n", buf, argc);
  puts(buf);

  /* ASCII payloads: the field-width form (which keeps the formatter path,
     since a raw write cannot pad) and the declined-bypass ordering case. */
  {
    char ascii[8];
    ascii[0] = 'o';
    ascii[1] = 'k';
    ascii[2] = '\0';
    printf("[%10s][%-10s]\n", ascii, ascii);
    printf("[%s] %d\n", ascii, noisy());
  }

  /* The same bytes through real file I/O, so they cannot be folded. */
  f = fopen("printf_string_nonascii_scratch.bin", "w");
  if (!f) {
    printf("open for write failed\n");
    return 1;
  }
  fwrite(buf, 1, 7, f);
  fwrite("\n", 1, 1, f);
  fclose(f);

  f = fopen("printf_string_nonascii_scratch.bin", "r");
  if (!f) {
    printf("open for read failed\n");
    return 1;
  }
  if (!fgets(line, 32, f)) {
    printf("read failed\n");
    return 1;
  }
  fclose(f);
  printf("file[%s]", line);
  return 0;
}

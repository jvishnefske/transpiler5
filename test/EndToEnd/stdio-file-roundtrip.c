// REQUIRES: cargo
// CTS-T1.3 (C99-48, c-testsuite 00187): differential regression test for
// real FILE* round-trip I/O over an owned handle backed by std::fs. The
// full 00187 shape is exercised: fopen("w") + byte-wise fwrite of a
// COMPUTED payload (data-dependent, not a literal), fclose, then serial
// reassignment of the SAME handle variable across four more fopen("r")
// passes: a size-1 fread of the first line, an fgetc EOF loop and a getc
// EOF loop each printing `ch: <n> '<c>'`, and an fgets line loop printing
// `x: <line>`. The fopen NULL-check path and the fwrite return count are
// both observable. The scratch filename is unique to this test because
// lit runs every EndToEnd binary in a SHARED per-directory cwd
// (build/test/EndToEnd), not a per-test directory; fopen("w") truncates,
// so a stale file from an earlier run cannot skew the result.
// Byte-identical stdout and a zero exit code against the clang-built
// native binary are required (a nonzero exit from either binary fails
// its RUN line).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name stdio_file_roundtrip_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stdio_file_roundtrip_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>

int main(void) {
  /* Data-dependent payload: two identical 6-byte lines ("world\n") whose
     characters are computed from offsets at runtime. */
  int offsets[5];
  char msg[12];
  int i;
  int j;
  offsets[0] = 22; /* w */
  offsets[1] = 14; /* o */
  offsets[2] = 17; /* r */
  offsets[3] = 11; /* l */
  offsets[4] = 3;  /* d */
  for (i = 0; i < 2; i++) {
    for (j = 0; j < 5; j++)
      msg[i * 6 + j] = 'a' + offsets[j];
    msg[i * 6 + 5] = '\n';
  }

  FILE *f = fopen("stdio_file_roundtrip_scratch_t13.txt", "w");
  if (!f) {
    printf("open for write failed\n");
    return 1;
  }
  int wrote = (int)fwrite(msg, 1, 12, f);
  printf("wrote: %d\n", wrote);
  fclose(f);

  /* Size-1 fread of the first line, mirroring 00187's fread check. */
  char head[7];
  f = fopen("stdio_file_roundtrip_scratch_t13.txt", "r");
  if (fread(head, 1, 6, f) != 6)
    printf("couldn't read scratch\n");
  head[6] = '\0';
  fclose(f);
  printf("%s", head);

  /* Byte loop via fgetc, EOF-terminated (00187 shape, uncast narrowing). */
  int in_ch;
  char show;
  f = fopen("stdio_file_roundtrip_scratch_t13.txt", "r");
  while ((in_ch = fgetc(f)) != EOF) {
    show = in_ch;
    if (show < ' ')
      show = '.';
    printf("ch: %d '%c'\n", in_ch, show);
  }
  fclose(f);

  /* Same loop again via getc. */
  f = fopen("stdio_file_roundtrip_scratch_t13.txt", "r");
  while ((in_ch = getc(f)) != EOF) {
    show = in_ch;
    if (show < ' ')
      show = '.';
    printf("ch: %d '%c'\n", in_ch, show);
  }
  fclose(f);

  /* Line loop via fgets (buffer of 7 holds "world\n" + NUL exactly). */
  f = fopen("stdio_file_roundtrip_scratch_t13.txt", "r");
  while (fgets(head, sizeof(head), f) != NULL)
    printf("x: %s", head);
  fclose(f);

  return 0;
}

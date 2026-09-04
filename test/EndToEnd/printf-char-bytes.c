// REQUIRES: cargo
// FR-194: the byte-level differential the `%c` family never had. C11
// 7.21.6.1p8 says `%c` converts its argument to `unsigned char` and writes
// THAT ONE CHARACTER, and 7.21.7.9 says the same of `putchar` -- exactly one
// byte for every value 0..255. The emitted crate used to route both through
// `__emitrust_fmt_c`, whose `(x as u8) as char` is a Latin-1 widening to a
// Unicode scalar, and `Display for char` writes UTF-8: every byte >= 0x80
// came out as TWO bytes. Measured on a 256-value sweep before the fix,
// native stdout was 256 bytes and the emitted crate's was 384, first
// differing at offset 0x80 -- while `emitrust-cc` exited 0 with empty stderr
// and `cargo build` was clean. That is the whole lesson: a compile-clean
// build and a FileCheck of the IR are both blind to this class, and an
// ASCII-only differential passes while the bug is live, which is exactly why
// the existing corpus missed it.
//
// This test therefore sweeps ALL 256 byte values through every `%c` shape the
// importer admits and compares the raw bytes, not the text: `cmp` for the
// byte-exact verdict and an `xxd` diff so a failure names the offset. The
// shapes, in order: `printf("%c")` over 0..255; `putchar` over 0..255 in a
// different permutation; a FIELD WIDTH (`%5c`/`%-5c`, where C pads the one
// byte to N columns -- N is a compile-time constant, so the padding is
// literal text around the raw write); `%c` interleaved with literal text and
// a `%d` hole in one format, so the raw byte write must be sequenced BETWEEN
// two `print!` segments; a `%c` call whose LATER argument writes stdout
// itself, which declines the bypass (C evaluates every argument before printf
// emits anything, so flushing a segment mid-directive would invert the order
// -- the same fence FR-191 had to add for `%s`, pinned here as observable
// bytes); `sprintf`/`snprintf` `"%c"`, which write into a char BUFFER and
// must report a LENGTH of one byte per directive, not two; `sprintf("%s")` of
// a buffer holding a high byte (the buffer position FR-191 recorded as still
// broken); and the CTS-S devirtualized `fprintf(stdout, ...)` alias, the
// second stdout `print!` position.
//
// Every payload byte is derived from `argc` and the test runs with three
// different argument vectors, so neither clang nor rustc can constant-fold
// the byte and hide a miscompile behind a folded literal.
// The scratch filename is unique to this test because lit runs every EndToEnd
// binary in a SHARED per-directory cwd (build/test/EndToEnd).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name printf_char_bytes_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/printf_char_bytes_e2e > %t.rust.out
// RUN: xxd %t.native.out > %t.native.hex
// RUN: xxd %t.rust.out > %t.rust.hex
// RUN: diff %t.native.hex %t.rust.hex
// RUN: cmp %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native.out
// RUN: %t.crate/target/release/printf_char_bytes_e2e a b > %t.rust.out
// RUN: xxd %t.native.out > %t.native.hex
// RUN: xxd %t.rust.out > %t.rust.hex
// RUN: diff %t.native.hex %t.rust.hex
// RUN: cmp %t.native.out %t.rust.out
// RUN: %t.native a b c d e > %t.native.out
// RUN: %t.crate/target/release/printf_char_bytes_e2e a b c d e > %t.rust.out
// RUN: xxd %t.native.out > %t.native.hex
// RUN: xxd %t.rust.out > %t.rust.hex
// RUN: diff %t.native.hex %t.rust.hex
// RUN: cmp %t.native.out %t.rust.out

#include <stdio.h>

/* CTS-S devirtualized alias of the hosted variadic `fprintf`: a direct call
   to a variadic function is a located rejection, so `fprintf(stdout, ...)`
   reaches the printf machinery only through a function pointer. That routing
   is the SECOND stdout `print!` position the raw-byte write has to cover. */
int (*fprintfptr)(FILE *, const char *, ...) = &fprintf;

static int seq;

/* A LATER printf argument that writes stdout itself. C evaluates every
   argument before printf emits anything, so this call's own output must land
   BEFORE the "[" of the call that passes it. Flushing a format segment
   mid-directive-scan would invert that, which is why the raw-byte bypass is
   declined when a later argument has side effects -- pinned here as
   observable BYTES rather than as IR. */
static int noisy(void) {
  seq = seq + 1;
  printf("<%d>", seq);
  return seq;
}

int main(int argc, char **argv) {
  /* Both destinations are generously sized: `[%5c][%-5c][%c]` writes 17
     bytes plus the terminator, and a destination too small for what sprintf
     writes is a C buffer overflow (undefined) that the emitted crate turns
     into a bounds-check panic -- a legal refinement, but not what this test
     is measuring. */
  char buf[64];
  char out[64];
  int i;
  int j;
  int k;
  int n;

  /* 1. printf("%c") over ALL 256 byte values, rotated by argc. Before the
        fix this alone turned 256 native bytes into 384. */
  for (i = 0; i < 256; i++)
    printf("%c", (i + argc) & 0xff);

  /* 2. putchar over all 256 values in a different permutation (7 is coprime
        with 256, so the multiplier still covers every residue). */
  for (i = 0; i < 256; i++)
    putchar((i * 7 + argc) & 0xff);

  /* 3. A field width: C pads the ONE byte to N columns with spaces, on the
        left by default and on the right under '-'. */
  for (i = 0; i < 8; i++)
    printf("[%5c][%-5c]", (0xc0 + i + argc) & 0xff, (0xc8 + i + argc) & 0xff);
  printf("\n");

  /* 4. %c interleaved with literal text and a %d hole, so the raw byte write
        lands between two format segments in program order. */
  for (i = 0; i < 4; i++)
    printf("a%cb%dc%c\n", (0xfc + i + argc) & 0xff, i * argc,
           (0x81 + i + argc) & 0xff);

  /* 5. The declined bypass: a LATER argument with side effects. The payload
        is ASCII (the funnel is byte-exact there); what this pins is the
        ORDER, which the bypass would have broken. */
  printf("[%c] %d\n", 'A' + argc, noisy());

  /* 6. The BUFFER positions. `sprintf`/`snprintf` do not write stdout, so the
        raw stdout helper is structurally unavailable to them; they must still
        store ONE byte per %c and return a length counting it once. Before the
        fix the buffer held the two-byte UTF-8 encoding and the return value
        was 2, so the corruption spread into every length-dependent
        computation downstream, not just into the bytes. `%5c`/`%-5c` ride
        along here because Rust pads a `char` by CHARACTER count, which is the
        C byte count only as long as the formatted string stays Latin-1. */
  /* The full 256-value sweep in the buffer position too, WITH a field width
     on both sides of the plain hole, printing the returned length and every
     byte the call wrote. A single sample would not have distinguished "the
     byte is right" from "the count is right"; before the fix both were
     wrong, and only for payloads >= 0x80. */
  for (i = 0; i < 256; i++) {
    n = sprintf(buf, "[%5c][%-5c][%c]", (i + argc) & 0xff, (i + argc) & 0xff,
                (i + argc) & 0xff);
    printf("%d:", n);
    for (j = 0; j < n + 1; j++)
      printf("%02x", (unsigned char)buf[j]);
    printf("\n");
  }

  /* snprintf's DEFINED truncation: every bound from 0 to 5 against a
     three-byte high-payload result, so the byte count that decides where the
     cut and the terminator land is exercised at, below and above the
     boundary. The returned length is the FULL formatted length either way. */
  for (i = 0; i < 4; i++) {
    for (j = 0; j < 6; j++) {
      out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = 'Z';
      n = snprintf(out, j, "%c%c%c", (0x80 + i + argc) & 0xff,
                   (0xfd - i) & 0xff, 'q');
      printf("s%d,%d:%d:", i, j, n);
      for (k = 0; k < 6; k++)
        printf("%02x", (unsigned char)out[k]);
      printf("\n");
    }
  }

  /* `%c` of a NUL: C writes the byte 0 and returns 1, and the terminator
     follows it. */
  n = sprintf(buf, "%c", 0);
  printf("sprintf-nul n=%d %02x %02x\n", n, (unsigned char)buf[0],
         (unsigned char)buf[1]);

  /* 7. `%s` in the same buffer position, over a char array holding a high
        byte: the shape FR-191 recorded as STILL BROKEN because the raw
        stdout helpers could not reach it. */
  buf[0] = (char)((0xc8 + argc) & 0xff);
  buf[1] = 'z';
  buf[2] = 0;
  n = sprintf(out, "<%s>", buf);
  printf("sprintf-s n=%d %02x %02x %02x %02x\n", n, (unsigned char)out[0],
         (unsigned char)out[1], (unsigned char)out[2], (unsigned char)out[3]);

  /* 8. The devirtualized fprintf(stdout, ...) alias. */
  fprintfptr(stdout, "%c%c\n", (0x80 + argc) & 0xff, (0xff - argc) & 0xff);
  return 0;
}

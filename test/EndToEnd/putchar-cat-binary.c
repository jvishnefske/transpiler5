// REQUIRES: cargo
// FR-194: the canonical `getchar`/`putchar` cat loop over BINARY input --
// the shape that makes the `%c` UTF-8 expansion a data-corrupting bug rather
// than a cosmetic one. `putchar(c)` writes `(unsigned char)c` as ONE byte
// (C11 7.21.7.9), so a cat loop is the identity on any byte stream; the
// emitted crate used to render the byte as `(x as u8) as char` and print it
// with `Display for char`, which is UTF-8, so every input byte >= 0x80 came
// out as two output bytes and the copy silently grew.
//
// The oracle is the raw byte stream, so the test never looks at text: `cmp`
// gives the byte-exact verdict and an `xxd` diff names the first differing
// offset when it fails. Three independent comparisons are made, and each
// catches something the others do not:
//   * native GENERATOR output vs emitted generator output -- 512 `putchar`
//     calls covering every one of the 256 byte values twice, in two
//     different permutations, with the payload derived from `argc` so no
//     constant folding on either side can see the bytes;
//   * native cat vs emitted cat over that same binary file -- the loop shape,
//     including the byte 0xff that a sign-extending `getchar` would confuse
//     with EOF and the NUL byte that a string-based reader would truncate on;
//   * the native cat output against its own input, which pins the ORACLE
//     itself: if cat is not the identity natively, the first two comparisons
//     would agree on a wrong answer.
// The generator lives in this same program (selected by argc) so the binary
// input needs no checked-in blob and no shell that can emit `\x` escapes.
// The scratch filename is unique to this test because lit runs every EndToEnd
// binary in a SHARED per-directory cwd (build/test/EndToEnd).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name putchar_cat_binary_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native gen > %t.native.gen
// RUN: %t.crate/target/release/putchar_cat_binary_e2e gen > %t.rust.gen
// RUN: xxd %t.native.gen > %t.native.genhex
// RUN: xxd %t.rust.gen > %t.rust.genhex
// RUN: diff %t.native.genhex %t.rust.genhex
// RUN: cmp %t.native.gen %t.rust.gen
// RUN: %t.native < %t.native.gen > %t.native.out
// RUN: %t.crate/target/release/putchar_cat_binary_e2e < %t.native.gen > %t.rust.out
// RUN: xxd %t.native.out > %t.native.hex
// RUN: xxd %t.rust.out > %t.rust.hex
// RUN: diff %t.native.hex %t.rust.hex
// RUN: cmp %t.native.out %t.rust.out
// RUN: cmp %t.native.gen %t.native.out
// RUN: %t.native gen a b c > %t.native.gen
// RUN: %t.crate/target/release/putchar_cat_binary_e2e gen a b c > %t.rust.gen
// RUN: cmp %t.native.gen %t.rust.gen
// RUN: %t.native < %t.native.gen > %t.native.out
// RUN: %t.crate/target/release/putchar_cat_binary_e2e < %t.native.gen > %t.rust.out
// RUN: cmp %t.native.out %t.rust.out
// RUN: cmp %t.native.gen %t.native.out

#include <stdio.h>

int main(int argc, char **argv) {
  int c;
  int i;

  /* Generator mode. Two passes over all 256 byte values in two different
     argc-dependent permutations: an additive rotation and an exclusive-or
     mask, both bijections on 0..255, so every byte value appears in both
     halves and neither half is a constant the optimizer can precompute. */
  if (argc > 1) {
    for (i = 0; i < 256; i++)
      putchar((i * 5 + argc) & 0xff);
    for (i = 255; i >= 0; i--)
      putchar((i ^ (argc * 37)) & 0xff);
    return 0;
  }

  /* Cat mode: the identity on any byte stream. getchar() yields 0..255 or
     -1, so 0xff must NOT be read as EOF, and the NUL bytes in the stream
     must pass through untouched. */
  while ((c = getchar()) != EOF)
    putchar(c);
  return 0;
}

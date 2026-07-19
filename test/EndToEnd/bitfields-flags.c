// REQUIRES: cargo
// C99-45: mixed bit-field runs under interleaved reads and writes. One
// struct packs an 8-bit enum field plus two 1-bit flags (one run), a
// plain int splitting the runs, and a signed 4-bit field with padding
// bits (second run). Flag writes are read-modify-writes that must not
// disturb the enum field or each other; the signed 4-bit field must
// sign-extend on read (-1 stays -1) and truncate on overflowing store
// (8 wraps to -8, clang's two's-complement behaviour on both sides).
// All stored values derive from argc so folding cannot pre-compute the
// digests; native and transpiled stdout must be byte-identical.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bitfields_flags > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern int printf(const char *, ...);

enum gear {
  G_IDLE = 3,
  G_RUN = 152, /* bit 7 set: must zero-extend out of the 8-bit field */
  G_STOP = 200
};

struct ctrl {
  enum gear g : 8;
  unsigned hot : 1;
  unsigned dirty : 1;
  int mid;
  int s : 4;
  unsigned pad : 3;
};

int main(int argc, char **argv) {
  struct ctrl c;

  c.mid = 100 + argc;
  c.g = (enum gear)(151 + argc); /* G_RUN at runtime */
  c.hot = argc & 1;              /* 1 */
  c.dirty = 0;
  printf("g %d\n", (int)c.g); /* 152 */

  c.dirty = c.hot; /* flag-to-flag copy through two accessors */
  c.s = -argc;     /* -1: stored as 0b1111 */
  printf("s %d\n", c.s); /* reads back -1, not 15 */
  c.s = 7 + argc; /* 8 overflows 4 signed bits: wraps to -8 */
  printf("s2 %d\n", c.s);

  c.hot = 0;
  printf("g2 %d\n", (int)c.g); /* still 152 after all flag traffic */
  printf("hot %u dirty %u\n", c.hot, c.dirty); /* 0 1 */
  printf("mid %d\n", c.mid); /* 101: the split member is untouched */

  switch (c.g) {
  case G_RUN:
    printf("match\n");
    break;
  default:
    printf("no\n");
    break;
  }
  return 0;
}

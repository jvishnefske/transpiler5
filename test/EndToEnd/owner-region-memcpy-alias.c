// REQUIRES: cargo
// Differential end-to-end test for the FR-140/141/142/146 SILENT-UNBUILDABLE
// class as it reaches the Phase-4 owner lift: a user helper taking two
// pointer parameters, called with two cursors into ONE promoted local array.
// Inside the emitted method both parameters are i64 indices into the SINGLE
// receiver region `self.data`, so the naive image took `&mut self.data[..]`
// and `&self.data[..]` at once -- rustc E0502 -- while emitrust-cc still
// exited 0 and wrote the crate. `planOwners`' all-or-nothing rule proves the
// two cursors index one whole region of known extent, which is exactly the
// condition the same-root array branch already rides `copy_within` on, so the
// pair joins it.
//
// The halves are deliberately DIFFERENT shapes and must not be conflated:
//   * `mvf`/`mvb` are GENUINELY OVERLAPPING memmoves -- [0,4) into [2,6) and
//     [3,7) into [1,5) -- which are well defined in C and are the case
//     `copy_within` exists for. They are the only overlapping copies this
//     file byte-diffs, because only memmove has a defined answer to diff
//     against.
//   * `cp`/`cpu` are memcpy over STRICTLY DISJOINT sub-ranges of one array
//     ([0,4) into [4,8)), where copy_within and a forward element walk agree
//     by construction. C's memcpy on OVERLAPPING ranges is undefined, so no
//     overlapping memcpy is byte-diffed here -- there would be no native
//     answer to be right about.
//   * `cx` is the control that must keep working: two cursors into two
//     DIFFERENT arrays, which unifies two storage bases, never promotes, and
//     keeps the ordinary two-slice image.
// One helper per array on purpose: a helper shared between two arrays would
// unify their storage bases and the class would never promote, so the shape
// under test would not be exercised at all.
// Every byte derives from argc so constant folding cannot hide a wrong
// direction, a shifted cursor, or a dropped copy, and `cp`'s count is a
// RUNTIME value for the same reason; `cargo build` success alone proves
// nothing for this class -- the stdout diff against the clang-built native is
// the oracle.
// Deterministic, no UB; main returns 0.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/owner_region_memcpy_alias > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/owner_region_memcpy_alias a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>
#include <string.h>

// Overlapping memmove, forward, through two pointer parameters of one region.
static void mvf(char *dst, const char *src, int n) {
  memmove(dst, src, (size_t)n);
}

// Overlapping memmove, backward, through two pointer parameters of one region.
static void mvb(char *dst, const char *src, int n) {
  memmove(dst, src, (size_t)n);
}

// Disjoint memcpy through two pointer parameters of one region.
static void cp(char *dst, const char *src, int n) {
  memcpy(dst, src, (size_t)n);
}

// The u8 image of the same shape.
static void cpu(unsigned char *dst, const unsigned char *src, int n) {
  memcpy(dst, src, (size_t)n);
}

// Disjoint memcpy whose two CURSORS are runtime values, not constants: the
// helper receives absolute element offsets into the receiver region, so a
// window shifted by one would show here and nowhere else.
static void cpr(char *dst, const char *src, int n) {
  memcpy(dst, src, (size_t)n);
}

// Control: the helper's parameters unify with TWO storage bases, so the
// class never promotes and both arguments stay independent slice borrows.
static void cx(char *dst, const char *src, int n) {
  memcpy(dst, src, (size_t)n);
}

int main(int argc, char **argv) {
  char a[8];
  char b[8];
  unsigned char u[8];
  char c[8];
  char e[8];
  char f[8];
  char r[16];
  int k = argc;
  int i;

  for (i = 0; i < 16; i++)
    r[i] = (char)(0x41 + i * argc);
  for (i = 0; i < 8; i++) {
    a[i] = (char)(0x41 + i * argc);
    b[i] = (char)(0x41 + i * argc);
    u[i] = (unsigned char)(0x81 + i * argc);
    c[i] = (char)(0x30 + i * argc);
    e[i] = (char)(0x50 + i * argc);
    f[i] = (char)(0x60 + i * argc);
  }

  /* Overlapping: source [0,4) lands on destination [2,6). */
  mvf(&a[2], &a[0], 4);
  for (i = 0; i < 8; i++)
    printf("%02x", (unsigned char)a[i]);
  printf("\n");

  /* Overlapping the other direction: source [3,7) lands on [1,5). */
  mvb(&b[1], &b[3], 4);
  for (i = 0; i < 8; i++)
    printf("%02x", (unsigned char)b[i]);
  printf("\n");

  /* Strictly disjoint memcpy inside one region, u8 element type. */
  cpu(&u[4], &u[0], 4);
  for (i = 0; i < 8; i++)
    printf("%02x", u[i]);
  printf("\n");

  /* Strictly disjoint memcpy with a RUNTIME count (2 or 4, always inside
     the destination sub-range), so the length is not a folded constant. */
  cp(&c[4], &c[0], argc + 1);
  for (i = 0; i < 8; i++)
    printf("%02x", (unsigned char)c[i]);
  printf("\n");

  /* Strictly disjoint memcpy at RUNTIME cursors: [k, k+4) into
     [8+k, 12+k), inside r for k in {1, 3}. */
  cpr(&r[8 + k], &r[k], 4);
  for (i = 0; i < 16; i++)
    printf("%02x", (unsigned char)r[i]);
  printf("\n");

  /* Control: two cursors into two DIFFERENT arrays. */
  cx(&e[0], &f[2], 3);
  for (i = 0; i < 8; i++)
    printf("%02x", (unsigned char)e[i]);
  printf("\n");

  return 0;
}

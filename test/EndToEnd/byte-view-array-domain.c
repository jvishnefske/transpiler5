// REQUIRES: cargo
// FR-229 Wave 2, capabilities D and E: the i8/u8 BYTE-ARRAY DOMAIN
// CROSSING, and the `(unsigned char *)&arr` spelling.
//
// C's `char` and `unsigned char` are one storage domain; the emitted Rust's
// `i8` and `u8` are two, and `emitrust.slice_of` will not bridge them --
// the verifier says so outright ("result slice element type 'ui8' does not
// match the base element type 'i8'"). So `char raw[N]` reaching a callee
// that wants `&[u8]` is lowered as a COPY-IN/COPY-OUT `[u8; N]` view with a
// per-byte `as u8`, which is bit-preserving in Rust, and -- because the
// view is a copy -- a MUTABLE parameter obliges a per-byte `as i8` write
// back after the call or the callee's writes are silently lost. That is
// the same measured miscompile class Wave 1 pinned for the scalar view
// (native -1431655766 against a naive Rust 1, from a crate that builds
// clean), so it is pinned here by stdout diff and not by `cargo build`.
//
// Capability E is the `&arr` SPELLING: `(unsigned char *)&raw` rather than
// `(unsigned char *)raw`. In C the two denote the same address; in the
// importer only the decay form had a lowering, so `&arr` was a flat
// rejection regardless of element type. Both spellings are exercised here,
// over both domains, so a lowering that quietly handled only one would
// print a short or empty line.
//
// What is pinned:
//  * THE BYTES ARE THE ARRAY'S BYTES. Every view is printed as hex, so a
//    dropped `as u8`, a reversed copy, a wrong length or a view of the
//    wrong object shows as a stdout difference.
//  * THE WRITE-BACK. The callee mutates the view and the C program then
//    reads the ORIGINAL `char` array. Without the copy-out the reads are
//    stale, and the crate still builds.
//  * A PARTIAL write through the view leaves the untouched bytes alone --
//    the case that catches a copy-out that reconstitutes from a stale
//    image rather than the mutated one.
//  * THE TWO CAPABILITIES COMPOSED with capability C, which is what the
//    corpus 037/038/039 family actually spells: `char raw[sizeof x];
//    memcpy(raw, &x, sizeof x); print_hex((unsigned char *)raw, ...)`.
//
// Seeds derive from `argc` so no constant fold can pre-compute a byte
// string. The program is deterministic and has no UB: every byte of every
// array is written before it is read, and no view outlives its call.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/byte_view_array_domain > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native x y > %t.native3.out
// RUN: %t.crate/target/release/byte_view_array_domain x y > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>
#include <string.h>

typedef struct {
  int floors;
  int bedrooms;
  double bathrooms;
} house_t;

static void print_hex(unsigned char *p, int len) {
  int i;
  for (i = 0; i < len; i++)
    printf("%02x", p[i]);
  printf("\n");
}

static void print_hex_const(const unsigned char *p, int len) {
  int i;
  for (i = 0; i < len; i++)
    printf("[%02x]", p[i]);
  printf("\n");
}

/* Writes every byte: the whole copy-out is exercised. */
static void zap(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    p[i] = (unsigned char)(0xa0 + i);
}

/* Writes ONE byte: everything else must come back unchanged. */
static void low_byte(unsigned char *p, int n) {
  (void)n;
  p[0] = 0xff;
}

/* Read-modify-write through the view. */
static void bump(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    p[i] = (unsigned char)(p[i] + 1);
}

static void print_chars(const char *tag, char *a, int n) {
  int i;
  printf("%s", tag);
  for (i = 0; i < n; i++)
    printf(" %d", (int)a[i]);
  printf("\n");
}

/* The corpus 037 driver verbatim: capability C then capability D. */
static void driver_int(int x) {
  char raw[sizeof(x)];
  memcpy(raw, &x, sizeof(x));
  print_hex((unsigned char *)raw, sizeof(raw));
}

/* The corpus 038 driver verbatim. */
static void driver_float(float x) {
  char raw[sizeof(x)];
  memcpy(raw, &x, sizeof(x));
  print_hex((unsigned char *)raw, sizeof(raw));
}

/* The corpus 039 driver verbatim: capability C, then E, then D. */
static void driver_house(int floors) {
  house_t house;
  char raw[sizeof(house)];
  house.floors = floors;
  house.bedrooms = 3;
  house.bathrooms = 2.;
  memcpy(raw, &house, sizeof(house));
  print_hex((unsigned char *)&raw, sizeof(raw));
}

int main(int argc, char **argv) {
  int seed = argc;
  char c4[4];
  char c8[8];
  unsigned char u4[4];
  int i;

  for (i = 0; i < 4; i++)
    c4[i] = (char)(0x70 + i * seed);
  for (i = 0; i < 8; i++)
    c8[i] = (char)(-1 - i * seed);
  for (i = 0; i < 4; i++)
    u4[i] = (unsigned char)(0xf0 + i * seed);

  /* D: the decay spelling, char[] into a &[u8] callee. */
  print_hex((unsigned char *)c4, sizeof(c4));
  print_hex((unsigned char *)c8, sizeof(c8));
  /* E: the &arr spelling, same arrays. */
  print_hex((unsigned char *)&c4, sizeof(c4));
  print_hex((unsigned char *)&c8, sizeof(c8));
  /* E over an array that is ALREADY in the u8 domain: no crossing, just
     the spelling. */
  print_hex((unsigned char *)&u4, sizeof(u4));
  print_hex((unsigned char *)u4, sizeof(u4));
  /* The shared (`const unsigned char *`) borrow of each. */
  print_hex_const((const unsigned char *)c4, sizeof(c4));
  print_hex_const((const unsigned char *)&c8, sizeof(c8));
  print_hex_const((const unsigned char *)&u4, sizeof(u4));

  /* The write-back obligation. Each array is read AFTER the call as its
     own `char`/`unsigned char` element type, so a lost copy-out prints
     the pre-call values. */
  zap((unsigned char *)c4, sizeof(c4));
  print_chars("zap c4:", c4, 4);

  low_byte((unsigned char *)&c8, sizeof(c8));
  print_chars("low c8:", c8, 8);

  bump((unsigned char *)c8, sizeof(c8));
  print_chars("bump c8:", c8, 8);

  zap((unsigned char *)&u4, sizeof(u4));
  print_hex(u4, 4);

  /* And the views must still agree after the write-backs. */
  print_hex((unsigned char *)&c4, sizeof(c4));
  print_hex((unsigned char *)c8, sizeof(c8));

  /* The corpus drivers, C + D + E composed. */
  driver_int(0x01020304 * seed + 5);
  driver_int(-seed);
  driver_float(1.25f * (float)seed);
  driver_house(seed);
  return 0;
}

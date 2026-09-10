// REQUIRES: cargo
// FR-229 Wave 2, capability C: `memcpy(dst, &obj, sizeof obj)` -- a memcpy
// whose SOURCE is the OBJECT REPRESENTATION of a scalar or a padding-free
// aggregate. The typed model has no bytes to copy FROM there, so the
// source is not resolved as a region at all: each scalar component is
// split with `T::to_ne_bytes` and scattered into the destination byte
// array at the component's C byte offset, in C declaration order. Rust's
// own layout of the struct is therefore irrelevant by construction, which
// is exactly why this is sound where a `transmute` is not.
//
// The invariant pinned here is that the copied bytes are the OBJECT's
// bytes, in the object's order, at the destination's cursor. The program
// prints them as hex, so a wrong width, a wrong endianness, a wrong field
// offset, or a copy landing at the wrong cursor shows up as a stdout
// difference against the clang-built native and NOT as a build failure --
// `cargo build` is compile-only and cannot see any of them.
//
// This file deliberately uses `unsigned char` destinations only, so what
// it pins is capability C ALONE; the i8/u8 domain crossing that the corpus
// 037/039 `char raw[]` destination also needs is pinned separately in
// byte-view-array-domain.c, and the two composed are pinned at the tail of
// that file.
//
// Covered: int/unsigned/short/long long/float/double sources; a
// padding-free `{int; int; double;}` (the corpus 039 struct verbatim); a
// copy at a NONZERO destination cursor inside a larger buffer; `memmove`
// through the same path; and two copies of the same object in a row, which
// catches a scatter that reads a stale image.
//
// Every seed derives from `argc`, so no constant fold can pre-compute the
// byte string on either side. The program is deterministic and has no UB:
// every destination byte that is ever read is written first, and every
// source object is fully initialized before its representation is taken.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/byte_view_memcpy_source > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b c > %t.native4.out
// RUN: %t.crate/target/release/byte_view_memcpy_source a b c > %t.rust4.out
// RUN: diff %t.native4.out %t.rust4.out

#include <stdio.h>
#include <string.h>

/* 4 + 4 + 8 == 16 == sizeof: padding-free. The corpus 039 struct. */
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

/* The corpus 037 shape with the destination already in the u8 domain, so
   only capability C is in play. */
static void driver_int(int x) {
  unsigned char raw[sizeof(x)];
  memcpy(raw, &x, sizeof(x));
  print_hex(raw, sizeof(raw));
}

/* The corpus 038 shape, same restriction. */
static void driver_float(float x) {
  unsigned char raw[sizeof(x)];
  memcpy(raw, &x, sizeof(x));
  print_hex(raw, sizeof(raw));
}

/* The corpus 039 shape, same restriction: the aggregate's fields must
   land at 0, 4 and 8 -- clang's own offsets, not rustc's. */
static void driver_house(int floors) {
  house_t house;
  unsigned char raw[sizeof(house)];
  house.floors = floors;
  house.bedrooms = 3;
  house.bathrooms = 2.;
  memcpy(raw, &house, sizeof(house));
  print_hex(raw, sizeof(raw));
}

int main(int argc, char **argv) {
  int seed = argc;
  int i32v = 0x01020304 * seed + 5;
  unsigned int u32v = 0xdeadbeefu - (unsigned int)seed;
  short i16v = (short)(-31000 + seed);
  long long i64v = -1234605616436508552LL + seed;
  float f = 1.25f * (float)seed;
  double d = -2.5 * (double)seed;
  house_t house;
  unsigned char raw[16];
  unsigned char window[24];
  int i;

  driver_int(i32v);
  driver_int(-i32v);
  driver_float(f);
  driver_house(seed);

  memcpy(raw, &i32v, sizeof(i32v));
  print_hex(raw, (int)sizeof(i32v));
  memcpy(raw, &u32v, sizeof(u32v));
  print_hex(raw, (int)sizeof(u32v));
  memcpy(raw, &i16v, sizeof(i16v));
  print_hex(raw, (int)sizeof(i16v));
  memcpy(raw, &i64v, sizeof(i64v));
  print_hex(raw, (int)sizeof(i64v));
  memcpy(raw, &f, sizeof(f));
  print_hex(raw, (int)sizeof(f));
  memcpy(raw, &d, sizeof(d));
  print_hex(raw, (int)sizeof(d));

  house.floors = seed;
  house.bedrooms = -seed;
  house.bathrooms = 0.5 * (double)seed;
  memcpy(raw, &house, sizeof(house));
  print_hex(raw, (int)sizeof(house));

  /* Twice in a row: the second copy must see the CURRENT value. */
  house.floors = house.floors + 1;
  memcpy(raw, &house, sizeof(house));
  print_hex(raw, (int)sizeof(house));

  /* A copy at a NONZERO destination cursor. Every byte of `window` is
     written before it is read, so nothing indeterminate is printed. */
  for (i = 0; i < 24; i++)
    window[i] = (unsigned char)(0x10 + i);
  memcpy(window + 4, &i32v, sizeof(i32v));
  memcpy(&window[12], &d, sizeof(d));
  print_hex(window, 24);

  /* memmove rides the same path. */
  memmove(raw, &i64v, sizeof(i64v));
  print_hex(raw, (int)sizeof(i64v));
  return 0;
}

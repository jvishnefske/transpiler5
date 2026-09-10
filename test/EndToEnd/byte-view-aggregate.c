// REQUIRES: cargo
// FR-229 Wave 1, capability B: the PADDING-FREE AGGREGATE byte view. The
// bytes are scattered per field at the field's C byte offset, in C
// declaration order, which is why Rust's own struct layout is irrelevant
// here and why this is sound where a `transmute` is not: nothing in the
// emitted crate depends on rustc laying `house_t` out the way clang did.
//
// The invariant pinned here is the OFFSET MAP. A `{int; int; double;}` on
// x86-64 is offsets 0/4/8 with size 16 and NO padding, so its object
// representation is fully determinate; getting a single field offset wrong
// (or emitting the fields in Rust's layout order rather than C's) produces
// a byte string that differs from the native's while still building
// cleanly, so `cargo build` cannot see it and only this diff can.
//
// EVERY struct viewed here is padding-free by clang's own `abi_layout`
// numbers (`size == sum(field sizes)` with contiguous offsets). Padded
// aggregates are a LOCATED rejection, not a test case, and deliberately so:
// C leaves padding bytes indeterminate, and the same clang -O0 binary
// prints `0100000007000000` for a `{char; int;}` on a clean stack and
// `01dddddd07000000` after the frame is dirtied. That is the byte-diff
// oracle's uninitialized-memory blind spot (FR-212), so admitting a padded
// aggregate would be unsound in a way this file could never detect --
// see byte-view-invalid.c for the pin that keeps it out.
//
// Seeds derive from `argc`; every field of every viewed object is assigned
// before the view is taken, and the callee only reads. Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/byte_view_aggregate > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

typedef struct {
  int floors;
  int bedrooms;
  double bathrooms;
} house_t;

/* 4 + 4 == 8 == sizeof: padding-free. */
typedef struct {
  int a;
  int b;
} pair_t;

/* 8 + 8 == 16 == sizeof: padding-free even though the alignment is 8. */
typedef struct {
  double x;
  double y;
} point_t;

/* 1 + 1 + 2 == 4 == sizeof: padding-free with mixed widths. */
typedef struct {
  unsigned char lo;
  unsigned char hi;
  unsigned short both;
} packed_t;

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

// The corpus 036 shape verbatim.
static void driver(int floors) {
  house_t house = {0};
  house.floors = floors;
  house.bedrooms = 3;
  house.bathrooms = 2.;
  print_hex((unsigned char *)&house, sizeof(house));
}

int main(int argc, char **argv) {
  pair_t pair;
  point_t point;
  packed_t packed;

  driver(argc);
  driver(-argc);

  pair.a = 0x11223344 + argc;
  pair.b = -argc;
  print_hex((unsigned char *)&pair, sizeof(pair));
  print_hex_const((const unsigned char *)&pair, sizeof(pair));

  point.x = 0.5 * (double)argc;
  point.y = -1024.25 * (double)argc;
  print_hex((unsigned char *)&point, sizeof(point));

  packed.lo = (unsigned char)(0xa0 + argc);
  packed.hi = (unsigned char)(0x0b + argc);
  packed.both = (unsigned short)(0xc0de + argc);
  print_hex((unsigned char *)&packed, sizeof(packed));

  // The view must see the CURRENT field values, not the ones at declaration.
  pair.a = pair.a + 1;
  print_hex((unsigned char *)&pair, sizeof(pair));
  return 0;
}

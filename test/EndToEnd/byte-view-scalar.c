// REQUIRES: cargo
// FR-229 Wave 1, capability A: the SCALAR byte view. `(unsigned char *)&x`
// at a byte-slice argument is the object representation of `x`, which the
// typed model never materializes; it lowers as `T::to_ne_bytes(x)` into a
// synthetic `[u8; sizeof(T)]` local that the callee then borrows. The
// invariant pinned here is that those bytes are the TARGET's bytes: the
// program prints them as hex, so a wrong width, a wrong endianness, or a
// wrong signedness mapping (`i32` vs `u32` in the ne_bytes callee name is
// harmless, but `i32` vs `i64` is not) shows up as a stdout difference
// against the clang-built native and NOT as a build failure. `cargo build`
// is compile-only and cannot see any of those.
//
// Every seed is derived from `argc` so no constant fold can pre-compute the
// byte string on either side, and the viewed objects cover both a
// PARAMETER (`driver`'s `x`, the corpus 034 shape) and a local. Widths
// 1/2/4/8 and both signednesses are covered, plus the const (`&[u8]`)
// borrow shape beside the mutable (`&mut [u8]`) one. The program is
// deterministic and has no UB: every viewed object is fully initialized
// before the view is taken, and the callees only read.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/byte_view_scalar > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

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

// The corpus 034 shape verbatim: the viewed object is the by-value
// parameter itself.
static void driver(int x) {
  print_hex((unsigned char *)&x, sizeof(x));
}

int main(int argc, char **argv) {
  int seed = argc;
  int i32v = 0x01020304 * seed + 5;
  unsigned int u32v = 0xdeadbeefu - (unsigned int)seed;
  long long i64v = -1234605616436508552LL + seed;
  unsigned short u16v = (unsigned short)(0xbeef + seed);
  short i16v = (short)(-31000 + seed);
  unsigned char u8v = (unsigned char)(0xa5 + seed);

  driver(i32v);
  driver(-i32v);

  print_hex((unsigned char *)&i32v, sizeof(i32v));
  print_hex((unsigned char *)&u32v, sizeof(u32v));
  print_hex((unsigned char *)&i64v, sizeof(i64v));
  print_hex((unsigned char *)&u16v, sizeof(u16v));
  print_hex((unsigned char *)&i16v, sizeof(i16v));
  print_hex((unsigned char *)&u8v, sizeof(u8v));

  print_hex_const((const unsigned char *)&i32v, sizeof(i32v));
  print_hex_const((const unsigned char *)&i64v, sizeof(i64v));

  // A view taken twice in a row must see the CURRENT value both times.
  i32v = i32v + 1;
  print_hex((unsigned char *)&i32v, sizeof(i32v));
  return 0;
}

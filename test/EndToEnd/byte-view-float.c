// REQUIRES: cargo
// FR-229 Wave 1, capability Af: the FLOAT byte view. `neBytesTypeName` was
// integer-only, so `(unsigned char *)&f` had no `to_ne_bytes` callee name
// to emit; `f32`/`f64::to_ne_bytes` need no dialect change and flow through
// `emitrust.call_opaque` unchanged. The invariant pinned here is that the
// IEEE-754 object representation the emitted crate prints is the one clang
// laid down -- a float byte view is the shape that catches a `f64` emitted
// where the C object is a `float` (or a silent promotion through the
// variadic printf path), because a promoted `double` prints EIGHT bytes
// where C has FOUR and a diff fires on the first line.
//
// Seeds come from `argc`, and every value is an EXACTLY representable
// binary fraction (quarters and integers) so the byte strings are
// bit-for-bit determined by IEEE-754 alone, with no rounding freedom on
// either side. Negative zero is included on purpose (FR-200's sign-of-zero
// class is invisible to a `==` comparison but not to a byte view), as are
// a negative value and a value with a full mantissa. The program is
// deterministic and has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/byte_view_float > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

static void print_hex(unsigned char *p, int len) {
  int i;
  for (i = 0; i < len; i++)
    printf("%02x", p[i]);
  printf("\n");
}

// The corpus 035 shape verbatim.
static void driver(float x) {
  print_hex((unsigned char *)&x, sizeof(x));
}

static void driverd(double x) {
  print_hex((unsigned char *)&x, sizeof(x));
}

int main(int argc, char **argv) {
  float seedf = (float)argc;
  double seedd = (double)argc;
  float f = 1.25f * seedf;
  double d = -2.5 * seedd;
  float nzero = -0.0f * seedf;
  float big = 16777216.0f + seedf; /* 2^24, the f32 integer ceiling */
  double dbig = 9007199254740992.0 + seedd;

  driver(f);
  driver(-f);
  driver(nzero);
  driver(big);
  driverd(d);
  driverd(dbig);

  print_hex((unsigned char *)&f, sizeof(f));
  print_hex((unsigned char *)&d, sizeof(d));
  print_hex((unsigned char *)&nzero, sizeof(nzero));
  return 0;
}

// REQUIRES: cargo
// Differential regression test for unsigned integer support: unsigned
// arithmetic with wrap-around, unsigned division/remainder/comparisons
// (which differ from their signed counterparts on high-bit values),
// unsigned bitwise and logical-vs-arithmetic right shifts, conversions
// between sign domains and widths, float conversions, unsigned literals,
// and switch on unsigned scrutinees including a ui64 case label above
// i64::MAX. Byte-identical stdout and exit codes against the clang-built
// native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/unsigned > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

long wrap_add(unsigned int a, unsigned int b) {
  // 4294967295u + 2u wraps to 1 in C; the Rust side must wrap identically.
  unsigned int c = a + b;
  return (long)c;
}

long wrap_mul(unsigned int a, unsigned int b) {
  unsigned int c = a * b;
  return (long)c;
}

long wrap_sub(unsigned int a, unsigned int b) {
  unsigned int c = a - b;
  return (long)c;
}

long udiv_urem(unsigned int a, unsigned int b) {
  // With the high bit set, unsigned and signed division disagree; this
  // catches any accidental signed lowering.
  return (long)(a / b) * 100000 + (long)(a % b);
}

int ucmp(unsigned int a, unsigned int b) {
  int r = 0;
  if (a < b) r += 1;
  if (a <= b) r += 2;
  if (a > b) r += 4;
  if (a >= b) r += 8;
  if (a == b) r += 16;
  if (a != b) r += 32;
  return r;
}

long ubits(unsigned int a, unsigned int b) {
  unsigned int r = (a & b) | (a ^ 0xF0F0F0F0u);
  r = r << 3;
  // Unsigned right shift is logical: high bits fill with zero.
  r = r >> 5;
  r &= ~b;
  return (long)r;
}

long widen_narrow(unsigned char c, unsigned short s, int neg) {
  // Zero-extension from unsigned ranks, reinterpretation of a negative
  // int, and truncation back down.
  unsigned long l = c;
  l = l + s;
  unsigned int u = neg; // reinterprets the negative bit pattern
  unsigned char back = (unsigned char)u;
  return (long)l * 10000 + (long)u % 1000 + (long)back;
}

double u2f(unsigned int u) {
  // As a double, 4294967295 is exact; a signed mislowering would yield -1.
  return (double)u;
}

long f2u(double d) {
  unsigned int u = (unsigned int)d;
  return (long)u;
}

int truthy(unsigned int u) {
  int n = 0;
  if (u) {
    n += 1;
  }
  while (u) {
    n += 2;
    u = u - u; // becomes 0, terminating the loop
  }
  return n;
}

int uswitch(unsigned int u) {
  switch (u) {
  case 0u:
    return 1;
  case 4000000000u: // above INT32_MAX
    return 2;
  default:
    return 0;
  }
}

int uswitch64(unsigned long long v) {
  switch (v) {
  case 0xFFFFFFFFFFFFFFFEull: // above INT64_MAX: bit-pattern match
    return 1;
  case 5ull:
    return 2;
  default:
    return 0;
  }
}

int uupdates(unsigned int u) {
  u += 7u;
  u -= 2u;
  u *= 3u;
  u /= 2u;
  u++;
  --u;
  unsigned int pre = ++u;
  unsigned int post = u++;
  return (int)(u % 1000000) + (int)(pre % 1000) + (int)(post % 1000);
}

int main(void) {
  printf("wrap_add=%ld\n", wrap_add(4294967295u, 2u));
  printf("wrap_mul=%ld\n", wrap_mul(3000000000u, 3u));
  printf("wrap_sub=%ld\n", wrap_sub(1u, 3u));
  printf("udiv_urem=%ld\n", udiv_urem(3000000007u, 10u));
  printf("ucmp_hi=%d ucmp_eq=%d ucmp_lo=%d\n", ucmp(3000000000u, 5u),
         ucmp(9u, 9u), ucmp(5u, 3000000000u));
  printf("ubits=%ld\n", ubits(0x12345678u, 0x0FF00FF0u));
  printf("widen_narrow=%ld\n", widen_narrow(200, 40000, -1));
  printf("u2f=%f\n", u2f(4294967295u));
  printf("f2u=%ld\n", f2u(4000000000.75));
  printf("truthy0=%d truthy1=%d\n", truthy(0u), truthy(77u));
  printf("usw=%d%d%d\n", uswitch(0u), uswitch(4000000000u), uswitch(7u));
  printf("usw64=%d%d%d\n", uswitch64(18446744073709551614ull),
         uswitch64(5ull), uswitch64(6ull));
  printf("uupdates=%d\n", uupdates(1000u));
  unsigned int loop = 0u;
  int steps = 0;
  do {
    loop += 3u;
    steps++;
  } while (loop < 10u);
  printf("loop=%ld steps=%d\n", (long)loop, steps);
  return 0;
}

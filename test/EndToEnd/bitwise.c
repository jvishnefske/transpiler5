// REQUIRES: cargo
// Differential regression test for signed bitwise and shift operators:
// and/or/xor/complement, left shifts, arithmetic right shifts of negative
// values, shift amounts of a different type than the shifted operand, and
// the bitwise/shift compound assignments. Byte-identical stdout and exit
// codes against the clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bitwise > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int ops(int a, int b) {
  return ((a & b) * 1000000) + ((a | b) * 1000) + (a ^ b);
}

int complement(int a) {
  return ~a;
}

long shifts(long w, int n) {
  // Arithmetic right shift of a negative value keeps the sign; the shift
  // amount is an int while the shifted operand is a long.
  long a = w << n;
  long b = w >> n;
  long c = (-w) >> 2;
  return a + b * 7 + c * 3;
}

int compound(int a, int n) {
  a &= 0x7F;
  a |= 0x100;
  a ^= 0x0F0;
  a <<= 2;
  a >>= n;
  long wide = a;
  wide <<= n; // long <<= int: narrower amount than the shifted operand
  return a + (int)wide;
}

int narrow(char c, short s) {
  // Narrow operands promote to int before the bit operations.
  return (c & s) + (c | s) + (c ^ s) + (~c) + (c << 4) + (s >> 3);
}

int main(void) {
  printf("ops=%d\n", ops(0x1234, 0x00FF));
  printf("comp0=%d comp1=%d\n", complement(0), complement(-8));
  printf("shifts=%ld\n", shifts(-1024, 3));
  printf("shifts2=%ld\n", shifts(99999, 11));
  printf("compound=%d\n", compound(0x3FF, 3));
  printf("narrow=%d\n", narrow(12, -300));
  int mask = 0;
  for (int i = 0; i < 8; i++) {
    if (i & 1) {
      mask |= 1 << i;
    }
  }
  printf("mask=%d\n", mask);
  return 0;
}

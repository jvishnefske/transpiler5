// REQUIRES: cargo
// C99-47: differential regression test for the extended printf directive
// grammar `%[flags][width][length]conv`. Adversarial values throughout:
// %x/%X/%o of -1 and INT_MIN (C prints the two's-complement bit pattern
// as unsigned; the transpiled u32 cast must match), width and zero-pad
// combos on negative numbers (C's zero pad is sign-aware), width narrower
// than the number, %u at UINT_MAX and %lu at ULONG_MAX, %ld with an
// unsigned long argument (the 00215 shape), %d with size_t sizeof values
// (the 00178/00184 shape, matching C's low-32-bit varargs read), long
// hex/octal at -1, and %c across the ASCII boundary bytes 0 < 32 < 65 <
// 126 < 127 plus an int-promoted char variable and a value above 255
// (converted to unsigned char like C). Byte-identical stdout and exit
// codes against the clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name printf_formats --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/printf_formats > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int main(void) {
  int intMin = -2147483647 - 1;
  int intMax = 2147483647;

  // Hex/octal of negative arguments print the bit pattern as unsigned.
  printf("%x %X %o\n", -1, -1, -1);
  printf("%x %X %o\n", intMin, intMin, intMin);
  printf("%x %X %o\n", 0, 0, 0);
  printf("%08x %08X %012o\n", 255, 255, 255);
  printf("[%12x][%-12x][%012x]\n", -305419896, -305419896, -305419896);

  // Width, left-align, and sign-aware zero pad on signed decimals.
  printf("[%5d][%-5d][%05d][%2d]\n", -42, -42, -42, -42);
  printf("[%5d][%-5d][%05d][%2d]\n", 42, 42, 42, 42);
  printf("[%05d][%011d][%020d]\n", intMin, intMin, intMin);
  printf("%d %i\n", intMin, intMax);

  // Unsigned decimals at the extremes, with width forms.
  unsigned int umax = 4294967295u;
  unsigned long ulmax = 18446744073709551615ul;
  printf("%u %lu\n", umax, ulmax);
  printf("[%10u][%-10u][%010u]\n", umax, umax, umax);

  // %ld with an unsigned long argument reads the same 64-bit pattern as
  // signed, exactly like C's varargs.
  unsigned long timeout = 2;
  printf("timeout=%ld\n", timeout);

  // %d with size_t arguments truncates to the low 32 bits like C on
  // x86-64.
  char ca;
  short sh;
  double db;
  printf("%d %d %d %d\n", sizeof(ca), sizeof(sh), sizeof(db), sizeof(!ca));

  // Long hex/octal of -1 prints all 64 bits.
  long ln = -1;
  printf("%lx %lX %lo %li\n", ln, ln, ln, ln);

  // %c across ASCII boundaries (0 emits a genuine NUL byte on both
  // sides); 321 converts to unsigned char 65 ('A').
  printf("[%c][%c][%c][%c][%c]\n", 0, 32, 65, 126, 127);
  printf("%c%c%c\n", 'h', 'i', '!');
  printf("%c\n", 321);
  char cv = 'q';
  printf("%c\n", cv);

  // Literal percent still passes through.
  printf("100%%\n");
  return 0;
}

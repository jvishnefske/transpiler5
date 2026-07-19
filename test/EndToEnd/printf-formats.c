// REQUIRES: cargo
// C99-47: differential regression test for the full printf directive
// grammar `%[flags][width][.precision][length]conv`: all five C99 flags,
// integer/string precision, the h/hh/ll length modifiers, and the
// f/F/e/E/g/G floating family (exact glibc parity, including the %#g
// rounding-carry quirk and non-finite spellings; NaN is exercised by the
// import-side development harness rather than here because the sign of a
// runtime 0.0/0.0 NaN is unspecified in C). Adversarial values throughout:
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

  // ---- C99-47 full-grammar extension ----

  // Integer precision: minimum digits, zero-padded after the sign; a zero
  // value with precision zero prints nothing; '0' is ignored next to a
  // precision (spaces pad instead).
  printf("[%.5d][%.0d][%.d][%.5d][%010.5d][%-10.5d|]\n", -42, 0, 0, intMin,
         -42, -42);
  printf("[%.12u][%.0u][%.5x][%.5o]\n", umax, 0, 255, 255);

  // Sign flags on signed decimals and their interaction with width and
  // zero padding.
  printf("[%+d][%+d][% d][% d][%+.5d][%+010d][% 010d][%+10.5d]\n", 42, -42,
         42, -42, intMin, 42, 42, 42);

  // '#' alternate forms: octal leading zero (only when needed), 0x/0X on
  // nonzero hex, prefixes inside the '0' width padding.
  printf("[%#x][%#X][%#o][%#x][%#o][%#.0o][%#.3o][%#010x][%-#10x|][%#8.5X]\n",
         255, intMin, 8, 0, 0, 0, 8, -305419896, 255, 255);

  // h/hh reduce the promoted argument to short/char range (masking, then
  // the ordinary rules).
  printf("[%hd][%hu][%hx][%#hx][%.7hd][%hhd][%hhu][%hhx][%+hhd][%08hhx]\n",
         65659, -1, -1, 305419896, -305419896, 321, -1, 321, 200, 321);

  // ll is 64-bit like l on this target.
  long long llmin = -9223372036854775807LL - 1;
  printf("[%lld][%llu][%llx][%#llo][%.25lld][%+lld]\n", llmin, -1LL, llmin,
         -1LL, llmin, 5LL);

  // %f with the full flags/width/precision matrix. Rounding is
  // half-to-even on the exact binary value on both sides (0.5 -> 0,
  // 1.5 -> 2, 2.5 -> 2), and %.20f prints the exact expansion of 0.1.
  printf("[%.0f][%.0f][%.0f][%.0f][%.20f]\n", 0.5, 1.5, 2.5, -0.5, 0.1);
  printf("[%.2f][%10.3f][%-10.3f|][%010.3f][%+.3f][% .3f][%+010.4f][%#.0f]\n",
         1.005, -3.14159, -3.14159, -3.14159, 2.5, 2.5, -2.5, 1.0);
  printf("[%.0f][%25.17f]\n", 1e300, 3.141592653589793);

  // %e/%E: exponent always signed with at least two digits; rounding
  // carries renormalize (9.5 at %.0e is 1e+01); denormals and huge
  // values keep exact digits.
  printf("[%e][%E][%.0e][%.0e][%e][%e][%.17e][%.2e]\n", 0.0, -0.0, 9.5, 1.0,
         5e-324, 1.7976931348623157e308, 0.1, -12345.6789);
  printf("[%10.2e][%-12.2e|][%012.2e][%+.2e][% e][%#.0e]\n", 2.5, 2.5, -2.5,
         2.5, 2.5, 1.0);

  // %g/%G: shortest form with trailing-zero trimming, the f/e style
  // switch at X < -4 or X >= P, and the glibc '#' rounding-carry quirk
  // (999999.5 gives "1.e+06" where an exact 10000 keeps its zeros).
  printf("[%g][%g][%g][%g][%g][%g][%g][%g]\n", 0.0, -0.0, 100000.0,
         1234567.0, 0.0001, 0.00001234, 5e-324, 0.1);
  printf("[%.0g][%.1g][%.3g][%.17g][%G][%+g][% g]\n", 9.5, 9.5, 999.9999,
         0.1, 1e-7, 42.5, 42.5);
  printf("[%#g][%#.4g][%#.4g][%#.3g][%#.0g][%10.3g][%-10.3g|][%010.3g]\n",
         999999.5, 9999.6, 10000.0, 100.0, 1.0, 0.0001, 0.0001, 0.0001);

  // Non-finite values: space padding even under '0', C spellings, case
  // from the conversion letter.
  double zero = 0.0;
  double inf = 1.0 / zero;
  printf("[%f][%F][%e][%E][%g][%G][%10f][%-10f|][%010f][%+f][% e]\n", inf,
         inf, -inf, inf, inf, -inf, inf, inf, inf, inf, inf);

  // %s precision truncates (literal at import time, arrays at runtime,
  // stopping at the array end or NUL under the bound), width right-aligns
  // by default like C.
  char buf[8] = "abcdef";
  char nz[3] = {'x', 'y', 'z'};
  printf("[%.3s][%.10s][%.0s][%.4s][%.9s][%.3s][%.2s]\n", "abcdef", "abc",
         "abc", buf, buf, nz, nz);
  printf("[%10s][%-10s|][%10.3s][%8.2s]\n", "abc", "abc", "abcdef", buf);

  // %c with width.
  printf("[%5c][%-5c|][%1c]\n", 65, 66, 67);
  return 0;
}

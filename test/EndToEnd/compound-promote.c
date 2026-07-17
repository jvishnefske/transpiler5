// REQUIRES: cargo
// Differential regression test for compound assignment with operand
// promotion (`char/short x; x += wider;`, CTS-S1): the LHS is loaded,
// widened to Sema's computation type, operated on, and narrowed back.
// The cases are adversarial against the zero-vs-sign-extension trap and
// against wrap-on-narrow: every division, remainder, and right shift on
// a top-bit-set narrow value prints a different result if the widen
// picks the wrong extension, and every overflowing +=/-=/*=/<<= prints
// a different result if the narrow does not truncate to the low bits.
// Also covers the 00111.c shape (short -= long), an int accumulator
// against a long long RHS, float/double promotion (the 00174.c shape),
// int-accumulator-times-double truncation toward zero, a promoted
// compound assignment to a global, and a value-position use.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/compound_promote > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

unsigned char g_byte = 0xF0;

void signed_char_cases(void) {
  // Sign-extension discriminators: with a zero-extended widen, -100/3
  // would compute 156/3 = 52 (not -33) and -2 >> 1 would compute
  // 254 >> 1 = 127 (not -1).
  signed char c = -100;
  c /= 3;
  printf("sc_div=%d\n", c);
  c = -100;
  c %= 7;
  printf("sc_rem=%d\n", c);
  c = -2;
  c >>= 1;
  printf("sc_shr=%d\n", c);
  // Wrap on narrow in both directions: 100 + 100 = 200 -> -56 and
  // -100 - 100 = -200 -> 56 once truncated to eight bits.
  c = 100;
  c += 100;
  printf("sc_add_wrap=%d\n", c);
  c = -100;
  c -= 100;
  printf("sc_sub_wrap=%d\n", c);
  // 65 << 2 = 260 -> 4 after truncation.
  c = 65;
  c <<= 2;
  printf("sc_shl_wrap=%d\n", c);
}

void unsigned_char_cases(void) {
  // Zero-extension discriminators: with a sign-extended widen, 0xF0/3
  // would compute -16/3 = -5 (not 80) and 0xFE >> 1 would compute
  // -2 >> 1 = -1 -> 255 (not 127).
  unsigned char u = 0xF0;
  u /= 3;
  printf("uc_div=%d\n", u);
  u = 0xF0;
  u %= 7;
  printf("uc_rem=%d\n", u);
  u = 0xFE;
  u >>= 1;
  printf("uc_shr=%d\n", u);
  // Wrap on narrow in both directions: 200 + 100 = 300 -> 44 and
  // 10 - 20 = -10 -> 246.
  u = 200;
  u += 100;
  printf("uc_add_wrap=%d\n", u);
  u = 10;
  u -= 20;
  printf("uc_sub_wrap=%d\n", u);
  // 16 * 17 = 272 -> 16 and 0x81 << 1 = 0x102 -> 2.
  u = 16;
  u *= 17;
  printf("uc_mul_wrap=%d\n", u);
  u = 0x81;
  u <<= 1;
  printf("uc_shl_wrap=%d\n", u);
}

void short_cases(void) {
  // The 00111.c shape: computation at long, narrow back to short.
  short s = 1;
  long l = 1;
  s -= l;
  printf("s_sub_long=%d\n", s);
  // Extension discriminators at 16 bits.
  s = -30000;
  s /= 7;
  printf("s_div=%d\n", s);
  s = -4;
  s >>= 1;
  printf("s_shr=%d\n", s);
  unsigned short us = 0x8000;
  us /= 3;
  printf("us_div=%d\n", us);
  us = 0xFFFE;
  us >>= 1;
  printf("us_shr=%d\n", us);
  // Wrap on narrow: 32767 + 1 -> -32768, 0xFFFF + 2 -> 1, 10 - 20 -> 65526.
  s = 32767;
  s += 1;
  printf("s_add_wrap=%d\n", s);
  us = 0xFFFF;
  us += 2;
  printf("us_add_wrap=%d\n", us);
  us = 10;
  us -= 20;
  printf("us_sub_wrap=%d\n", us);
}

void int_with_long_long(void) {
  // int accumulator, long long computation type: with a zero-extended
  // widen, -100 would become 4294967196 and the quotient/remainder flip.
  int i = -100;
  long long d = 7;
  i /= d;
  printf("i_div_ll=%d\n", i);
  i = -100;
  i %= d;
  printf("i_rem_ll=%d\n", i);
  // Wrap on narrow from 64 to 32 bits: 2^31 - 1 + 2 -> -2^31 + 1.
  int w = 2147483647;
  long long two = 2;
  w += two;
  printf("i_add_ll_wrap=%d\n", w);
  // Shift amount of long long type exercises the width normalization.
  int sh = -8;
  long long amount = 2;
  sh >>= amount;
  printf("i_shr_ll=%d\n", sh);
}

void float_cases(void) {
  // The 00174.c shape: float accumulator, double computation type.
  float a = 12.34f;
  a += 56.78;
  printf("f_add=%f\n", a);
  a = 12.34f;
  a -= 56.78;
  printf("f_sub=%f\n", a);
  a = 12.34f;
  a *= 56.78;
  printf("f_mul=%f\n", a);
  a = 12.34f;
  a /= 56.78;
  printf("f_div=%f\n", a);
  // int accumulator, double computation type: C truncates toward zero,
  // so -7 / 2.0 = -3.5 prints -3 (a floor-style narrow would print -4)
  // and 7 * 2.5 = 17.5 prints 17.
  int i = -7;
  i /= 2.0;
  printf("i_div_f=%d\n", i);
  i = 7;
  i *= 2.5;
  printf("i_mul_f=%d\n", i);
}

void global_and_value_position(void) {
  // Promoted compound assignment straight to a global (the direct-global
  // fast path): 0xF0 / 3 = 80 requires the zero-extended widen.
  g_byte /= 3;
  printf("g_div=%d\n", g_byte);
  g_byte += 200;
  printf("g_add_wrap=%d\n", g_byte);
  // Value-position use observes the narrowed, wrapped stored value.
  unsigned char u = 200;
  int r = (u += 100);
  printf("value_pos=%d %d\n", r, u);
}

int main(void) {
  signed_char_cases();
  unsigned_char_cases();
  short_cases();
  int_with_long_long();
  float_cases();
  global_and_value_position();
  return 0;
}

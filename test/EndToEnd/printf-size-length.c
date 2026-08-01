// REQUIRES: cargo
// Stage-C regression: the `z`/`j`/`t` printf length modifiers
// (size_t/intmax_t/ptrdiff_t) are 64-bit on the LP64 x86-64 oracle target and
// behave like `l`/`ll` on the integer conversions. Byte-matches the
// clang-native build across %zu/%zd/%zx and the j/t forms. main returns 0 and
// reports via printf.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/printf_size_length > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int main(void) {
  unsigned long a = 4000000000UL;
  long b = -123456789012L;
  unsigned long c = 0x2a;
  printf("%zu %zd %zx\n", a, b, c);
  printf("%ju %jd\n", (unsigned long)7u, (long)-9);
  printf("%tu %td\n", (unsigned long)3u, (long)-4);
  // Width/flags still apply on top of the length.
  printf("[%08zx] [%-6zu|]\n", c, a);
  return 0;
}

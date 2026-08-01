// REQUIRES: cargo
// Stage-C regression: `snprintf(dest, size, fmt, ...)` shares the sprintf
// lowering but honors the size bound via the truncating `__emitrust_snprintf`
// helper -- it writes at most `size - 1` formatted bytes plus a NUL (C's
// DEFINED truncation) and returns the FULL formatted length regardless of
// truncation. Byte-matches the clang-native build across a fitting write, a
// truncating write, and a size-0 write. main returns 0 and reports via printf.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/snprintf > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);
int snprintf(char *, unsigned long, const char *, ...);

int main(void) {
  char buf[16];
  int n1 = snprintf(buf, sizeof buf, "n=%d s=%s", 42, "hi");
  printf("[%s] n1=%d\n", buf, n1);

  char small[4];
  int n2 = snprintf(small, sizeof small, "%s", "abcdef");
  printf("[%s] n2=%d\n", small, n2); // truncated to 3 chars, returns 6

  char one[8] = {'Z', 0, 0, 0, 0, 0, 0, 0};
  int n3 = snprintf(one, 0, "%d", 999); // size 0: writes nothing
  printf("[%s] n3=%d\n", one, n3);

  return 0;
}

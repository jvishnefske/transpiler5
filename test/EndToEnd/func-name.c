// REQUIRES: cargo
// C99-29: differential end-to-end test for __func__ (and the
// __FUNCTION__/__PRETTY_FUNCTION__ synonyms): printed via printf %s and
// puts, bound to a const char pointer and walked byte by byte, and
// measured with strlen. Byte-identical stdout and exit codes against the
// clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name func_name_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/func_name_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);
int puts(const char *);
int strlen(const char *);

void announce(void) {
  printf("in %s\n", __func__);
  puts(__FUNCTION__);
}

int name_length(void) {
  return strlen(__func__);
}

int walked_length(void) {
  const char *p = __func__;
  int n = 0;
  while (*p != 0) {
    n = n + 1;
    p++;
  }
  return n;
}

int main(void) {
  announce();
  printf("%s says %d and %d\n", __func__, name_length(), walked_length());
  return 0;
}

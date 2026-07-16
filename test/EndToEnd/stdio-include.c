// REQUIRES: cargo
// C99-39: differential end-to-end test for a real `#include <stdio.h>`. The
// header's declarations are skipped at import time (system-header policy)
// and printf lowers by name, so the program must behave identically to the
// inline-prototype variants used by the other EndToEnd tests. The natively
// compiled C binary and the transpiled crate must produce byte-identical
// stdout. --release matches the other EndToEnd tests: debug Rust panics on
// integer overflow where C wraps; every value here stays small.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name stdio_include --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stdio_include > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>

int square(int x) { return x * x; }

int main(void) {
  int total = 0;
  for (int i = 1; i <= 5; ++i) {
    total = total + square(i);
    printf("i=%d square=%d total=%d\n", i, square(i), total);
  }
  printf("done %d\n", total);
  return 0;
}

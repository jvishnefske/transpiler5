// REQUIRES: cargo
// FR-21: differential end-to-end test: transpile to a cargo crate, build it, and
// compare its stdout against the natively compiled C program. main returns
// 0 and reports everything via printf, so lit's per-command exit-code
// checking covers both runs and diff covers the observable behavior.
// --release is load-bearing: debug Rust panics on integer overflow where C
// wraps. The program below is deterministic, has no UB, and keeps all
// values comfortably small so no overflow can occur in either language.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/loops > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int main(void) {
  int total = 0;
  for (int i = 0; i < 6; ++i) {
    if (i == 4) {
      break;
    }
    int j = 0;
    while (j < 5) {
      j = j + 1;
      if (j == 3) {
        continue;
      }
      total = total + i * 10 + j;
      printf("i=%d j=%d total=%d\n", i, j, total);
    }
  }
  printf("final=%d\n", total);
  return 0;
}

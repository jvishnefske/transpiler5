// REQUIRES: cargo
// C99-35 pin, differential end-to-end test: recursion. A directly
// recursive function (sum_to) is called with a data-dependent depth — the
// depth is computed at runtime from loop-accumulated values, not a
// literal — and a mutually recursive pair (is_even/is_odd calling each
// other) walks down a computed argument. Results are printed and diffed
// against the native build. All depths are small and bounded, all
// arithmetic stays far from overflow, and there is no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/recursion > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int sum_to(int n) {
  if (n <= 0)
    return 0;
  return n + sum_to(n - 1);
}

int is_odd(int n);

int is_even(int n) {
  if (n == 0)
    return 1;
  return is_odd(n - 1);
}

int is_odd(int n) {
  if (n == 0)
    return 0;
  return is_even(n - 1);
}

int main(void) {
  int depth = 0;
  for (int i = 1; i <= 4; ++i) {
    depth = depth + i;
  }
  printf("depth %d\n", depth);
  printf("sum %d\n", sum_to(depth));
  printf("sum2 %d\n", sum_to(depth * 2));

  for (int i = 0; i < 3; ++i) {
    int probe = depth + i;
    printf("parity %d %d %d\n", probe, is_even(probe), is_odd(probe));
  }
  return 0;
}

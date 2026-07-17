// REQUIRES: cargo
// C99-14/C99-15: differential end-to-end test: a mutable global counter
// mutated across calls, a function-local static accumulator, a const
// global, a zero-initialized global array read and written element-wise, a
// global double, tentative/extern reconciliation, and a mutable global
// named `c` (CTS-E1: the accessor closure binder is `__emitrust_tl`, so
// the emitted pattern must not resolve against the `c` thread-local key
// and fail rustc). main returns 0 and
// reports everything via printf, so lit's per-command exit-code checking
// covers both runs and diff covers the observable behavior.
// --release is load-bearing: debug Rust panics on integer overflow where C
// wraps. The program below is deterministic, has no UB, keeps all values
// comfortably small, and puts every side-effecting call in its own
// statement so no unspecified evaluation order is exercised.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/globals > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int counter;             /* tentative definition: zero-initialized */
int counter;             /* repeated tentative definition */
extern int limit;        /* extern declaration reconciled below */
int limit = 50;
const int scale = 3;     /* immutable global */
double ratio = 2.5;
int table[4];            /* zero-initialized global array */
int c;                   /* CTS-E1: name must not shadow the accessor binder */

int bump(void) {
  counter += scale;
  return counter;
}

int accumulate(int x) {
  static int total = 7;  /* static local preserving state across calls */
  total += x;
  return total;
}

void fill_table(void) {
  for (int i = 0; i < 4; ++i) {
    table[i] = table[i] + (i + 1) * scale;
  }
}

int table_sum(void) {
  int s = 0;
  for (int i = 0; i < 4; ++i) {
    s += table[i];
  }
  return s;
}

int main(void) {
  printf("start counter=%d limit=%d scale=%d\n", counter, limit, scale);
  for (int i = 0; i < 5; ++i) {
    int b = bump();
    int a = accumulate(i);
    printf("i=%d bump=%d acc=%d\n", i, b, a);
  }
  fill_table();
  fill_table();
  for (int i = 0; i < 4; ++i) {
    printf("table[%d]=%d\n", i, table[i]);
  }
  table[2] = table_sum();
  counter = counter + table[2];
  limit = limit + counter;
  ratio = ratio * 2.0;
  c = counter - limit;
  c = c + table[0];
  printf("counter=%d limit=%d sum=%d ratio=%f c=%d\n", counter, limit,
         table_sum(), ratio, c);
  return 0;
}

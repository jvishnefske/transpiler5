// REQUIRES: cargo
// FR-62 slice 4 (stage A) differential test, the GLOBALS shape: under
// --actor-lift the multi-global cluster {counter, table} (+ const scale)
// becomes CounterActor with bump/fill_table/table_sum as &mut-self
// methods, accumulate's function-local static becomes its own
// AccumulateActor, and the main-only globals limit/ratio/c become named
// main locals — so NO thread_local survives, yet the observable behavior
// is byte-identical to the clang-built native (stdout + exit code, THE
// oracle). This is test/EndToEnd/globals.c verbatim with the flag on: the
// same program that pins today's thread-local form there pins the actor
// form here, so a lift bug diverges one of the two.
// --release is load-bearing: debug Rust panics on integer overflow where C
// wraps.
// RUN: emitrust-cc --actor-lift --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/actor_lift_globals > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int counter;             /* tentative definition: zero-initialized */
int counter;             /* repeated tentative definition */
extern int limit;        /* extern declaration reconciled below */
int limit = 50;
const int scale = 3;     /* immutable global */
double ratio = 2.5;
int table[4];            /* zero-initialized global array */
int c;                   /* main-only global named like the accessor binder */

int bump(void) {
  counter += scale;
  return counter;
}

int accumulate(int x) {
  static int total = 7;  /* static local: becomes AccumulateActor{total} */
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

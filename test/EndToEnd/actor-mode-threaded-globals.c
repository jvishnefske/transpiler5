// REQUIRES: cargo
// FR-62 slice 5b differential test, the THREADED shape: under
// --actor-mode=threaded the SLICE-5b SPIKE's program (actor-lift-globals.c
// verbatim) puts every eligible actor's state on a real std::thread behind
// an mpsc mailbox — CounterActor (the landed plan's merged counter+table
// cluster, 3 arms + synthesized get_/set_ accessors for main's direct
// member/subscript accesses) and AccumulateActor (the lifted static local,
// its total=7 initializer becoming a set_total call after spawn) — with a
// per-call typed reply channel keeping every call synchronous, so the
// observable behavior is byte-identical to the clang-built native (stdout
// + exit code, THE oracle). The greps prove the threading actually fired:
// the handle aliases and the shared runtime are in the crate, and no
// thread_local and no unsafe survive. --release is load-bearing: debug
// Rust panics on integer overflow where C wraps.
// RUN: emitrust-cc --actor-mode=threaded --emit=crate %s -o %t.crate --build
// RUN: grep "type CounterActorHandle = actor_rt::Handle<CounterActorMsg>;" %t.crate/src/main.rs
// RUN: grep "type AccumulateActorHandle = actor_rt::Handle<AccumulateActorMsg>;" %t.crate/src/main.rs
// RUN: grep "mod actor_rt" %t.crate/src/main.rs
// RUN: not grep "thread_local" %t.crate/src/main.rs
// RUN: not grep "unsafe" %t.crate/src/main.rs
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/actor_mode_threaded_globals > %t.rust.out
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

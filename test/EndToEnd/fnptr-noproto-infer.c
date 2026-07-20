// REQUIRES: cargo
// FR-29 / CTS 00209: K&R callsite-prototype inference, differential
// end-to-end. Unlike 00209 itself (whose inferred calls are dead code),
// every inferred call here EXECUTES with data-dependent arguments:
// the f1 trampoline shape dispatched over two targets, a local no-proto
// pointer driven by loop data and reassigned between agreeing call
// sites, and the char->int and float->double default-promotion slots.
// main returns 0 and reports everything via printf, so lit's
// per-command exit-code checking covers both runs and diff covers the
// observable behavior. Every called pointer is non-null; no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/fnptr_noproto_infer > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int twice(int x) { return x + x; }
int negate(int x) { return -x; }
int addc(int c) { return c + 1; }
int halve(double d) { return (int)(d / 2.0); }

typedef int (*fptr)();

/* The 00209 f1 shape, executed: the pointee is inferred from the
   (*fp)(i) call site and invoked with runtime-selected targets. */
int trampoline(fptr fp, int i) { return (*fp)(i); }

int main(void) {
  int (*np)() = twice;
  int (*cp)() = addc;
  int (*dp)() = halve;
  char c = 'A';
  float f = 9.0f;
  int acc = 0;
  int i;

  printf("%d\n", trampoline(twice, 21));
  printf("%d\n", trampoline(negate, 7));

  /* Loop-fed arguments through the inferred local, then a reassignment
     between agreeing call sites feeding the accumulated value back. */
  for (i = 1; i <= 3; i++)
    acc += np(i);
  np = negate;
  acc += np(acc);
  printf("%d\n", acc);

  /* char argument: default-promoted to int at the inferred call. */
  printf("%d\n", cp(c));

  /* float argument: default-promoted to double at the inferred call. */
  printf("%d\n", dp(f));
  return 0;
}

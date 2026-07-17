// REQUIRES: cargo
// Differential regression test for (void) casts and void-typed contexts:
// a discarded call still runs (and its output is observable), a discarded
// assignment still stores, a discarded pure expression compiles to
// nothing, and a void-typed conditional in statement position runs only
// the selected arm's side effects. Byte-identical stdout and exit codes
// against the clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/void_cast > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int global;

int bump(int amount) {
  global = global + amount;
  return global;
}

int main(void) {
  int x = 5;
  // Discarded call: the side effect on `global` must survive.
  (void)bump(7);
  printf("global=%d\n", global);
  // Discarded assignment and increment.
  (void)(x = x + 10);
  (void)x++;
  printf("x=%d\n", x);
  // Discarded pure expression: no observable effect, must still compile.
  (void)(x * global);
  (void)x;
  // Discarded printf keeps printing.
  (void)printf("discarded printf x=%d\n", x);
  // Void-typed conditional in statement position: only the selected arm
  // runs.
  x > 0 ? (void)bump(100) : (void)bump(-100);
  x < 0 ? (void)bump(1000) : (void)bump(-1000);
  printf("global=%d\n", global);
  return global == -893 ? 0 : 1;
}

// REQUIRES: cargo
// FR-25: adversarial loop-carried rotation vectors for the lost-copy
// problem of SSA destruction. Every loop below has a back-edge whose
// block-argument rebinding forms a copy cycle, so sequential emission of
// the rebinding (instead of parallel-assignment semantics) reads a
// clobbered carried variable and changes the printed output:
// - swap2: a pure 2-cycle (a, b = b, a) over an odd iteration count, so a
//   net swap must be observable; sequential emission collapses both
//   variables to the same value.
// - fib3: the Fibonacci-shaped 3-variable rotation from the found-in-the-
//   wild miscompile.
// - partial: only two of four carried variables cycle (a swap); the other
//   two are straight-line updates whose sources are freshly computed
//   values and must be left untouched by the cycle-breaking.
// - mixed: a 3-rotation carried alongside independent accumulator and
//   counter updates in the same loop.
// Differential test: transpile to a cargo crate, build it, and diff its
// stdout against the natively compiled C program. Deterministic, no UB,
// all values stay far from overflow.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/lostcopy_rotations > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

void swap2(void) {
  int a = 1;
  int b = 2;
  int i = 0;
  while (i < 5) {
    int t = a;
    a = b;
    b = t;
    i = i + 1;
  }
  printf("swap2 a=%d b=%d\n", a, b);
}

void fib3(void) {
  int a = 0;
  int b = 1;
  int c = 1;
  while (c < 100) {
    a = b;
    b = c;
    c = a + b;
  }
  printf("fib3 a=%d b=%d c=%d\n", a, b, c);
}

void partial(void) {
  int x = 3;
  int y = 8;
  int sum = 0;
  int n = 0;
  while (n < 7) {
    int t = x;
    x = y;
    y = t;
    sum = sum + x;
    n = n + 1;
  }
  printf("partial x=%d y=%d sum=%d n=%d\n", x, y, sum, n);
}

void mixed(void) {
  int a = 2;
  int b = 5;
  int c = 1;
  int sum = 0;
  int count = 0;
  while (count < 6) {
    a = b;
    b = c;
    c = a + b;
    sum = sum + a - b;
    count = count + 1;
  }
  printf("mixed a=%d b=%d c=%d sum=%d count=%d\n", a, b, c, sum, count);
}

int main(void) {
  swap2();
  fib3();
  partial();
  mixed();
  return 0;
}

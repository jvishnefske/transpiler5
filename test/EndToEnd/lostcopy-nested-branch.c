// REQUIRES: cargo
// FR-25: lost-copy rotation vectors in composed control flow:
// - nested: an outer while loop rotating one pair of carried variables
//   with an inner while loop rotating another pair on every outer
//   iteration; both back-edges carry copy cycles and the outer rotation
//   is seeded from the inner loop's result, so a sequential rebinding at
//   either level changes the printed output.
// - branchy: a 3-variable rotation whose freshly rotated values steer a
//   data-dependent branch inside the same loop; a clobbered rotation
//   flips branch outcomes and diverges both accumulators.
// Differential test: transpile to a cargo crate, build it, and diff its
// stdout against the natively compiled C program. Deterministic, no UB,
// all values stay far from overflow.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/lostcopy_nested_branch > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

void nested(void) {
  int p = 1;
  int q = 4;
  int outer = 0;
  while (outer < 3) {
    int u = p;
    int v = q + outer;
    int inner = 0;
    while (inner < 3) {
      int t = u;
      u = v;
      v = t + 1;
      inner = inner + 1;
    }
    int t = p;
    p = q + u;
    q = t + v;
    outer = outer + 1;
    printf("nested outer=%d p=%d q=%d u=%d v=%d\n", outer, p, q, u, v);
  }
  printf("nested final p=%d q=%d\n", p, q);
}

void branchy(void) {
  int a = 0;
  int b = 1;
  int c = 1;
  int low = 0;
  int high = 0;
  while (c < 60) {
    a = b;
    b = c;
    c = a + b;
    if (b > a + 1) {
      high = high + b;
    } else {
      low = low + a;
    }
  }
  printf("branchy a=%d b=%d c=%d low=%d high=%d\n", a, b, c, low, high);
}

int main(void) {
  nested();
  branchy();
  return 0;
}

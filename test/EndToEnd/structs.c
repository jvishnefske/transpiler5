// REQUIRES: cargo
// FR-21: differential end-to-end test: structs by pointer and by value, an int
// array indexed in loops, and address-of. main returns 0 and reports
// everything via printf, so lit's per-command exit-code checking covers
// both runs and diff covers the observable behavior.
// --release is load-bearing: debug Rust panics on integer overflow where C
// wraps. The program below is deterministic, has no UB, and keeps all
// values comfortably small so no overflow can occur in either language.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/structs > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct Point {
  int x;
  int y;
};

void scale(struct Point *p, int f) {
  p->x = p->x * f;
  p->y = p->y * f;
}

int manhattan(struct Point q) {
  return q.x + q.y;
}

int main(void) {
  int vals[4];
  for (int i = 0; i < 4; ++i) {
    vals[i] = (i + 1) * (i + 2);
  }
  int s = 0;
  for (int i = 0; i < 4; ++i) {
    s = s + vals[i];
    printf("vals[%d]=%d s=%d\n", i, vals[i], s);
  }
  struct Point p;
  p.x = vals[0] + vals[1];
  p.y = vals[2] + vals[3];
  printf("x=%d y=%d\n", p.x, p.y);
  scale(&p, 3);
  printf("x=%d y=%d\n", p.x, p.y);
  int m = manhattan(p);
  printf("m=%d\n", m);
  return 0;
}

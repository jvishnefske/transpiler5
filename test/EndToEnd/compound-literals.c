// REQUIRES: cargo
// C99-13: compound literals in expression position, differential
// end-to-end: transpile to a cargo crate, build it, and compare its stdout
// against the natively compiled C program. Adversarial shapes on purpose:
// a self-referencing assignment (`s = (struct S){s.b, s.a}` must read the
// old values through the temp — the lost-copy swap shape), member and
// subscript directly on the literal, whole-struct value uses (argument,
// return, assignment), a walked pointer into an array literal with writes
// through it, a degenerate struct-literal base mutated through the
// pointer, a loop-local literal whose hole is dirtied each iteration (the
// next evaluation must restore the C99 zero fill), conditional rebinding
// across two literals (multi-base region), a designated partial literal,
// a nested literal as an aggregate element, and a string-filled char
// array literal. main returns 0 and reports everything via printf, so
// lit's per-command exit-code checking covers both runs and diff covers
// the observable behavior. The program is deterministic and has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/compound_literals > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct S {
  int a;
  int b;
};
struct T {
  struct S inner;
  int c;
};

static int sum(struct S s) { return s.a * 100 + s.b; }
static struct S make(int k) { return (struct S){k, -k}; }
static int first_two(int *p) { return p[0] * 10 + p[1]; }

int main(void) {
  /* Initializer copy. */
  struct S s = (struct S){1, 2};
  printf("init %d %d\n", s.a, s.b);

  /* Self-referencing assignment: the temp is built before s changes. */
  s = (struct S){s.b, s.a};
  printf("swap %d %d\n", s.a, s.b);

  /* Member and subscript directly on the literal. */
  printf("member %d\n", (struct S){5, -6}.b);
  int idx = 2;
  printf("subscript %d\n", (int[]){7, 8, -9}[idx]);
  printf("deref %d\n", *(int[]){11, -12});

  /* By-value argument and returned struct. */
  printf("arg %d\n", sum((struct S){3, -4}));
  struct S r = {0, 0};
  r = make(21);
  printf("ret %d %d\n", r.a, r.b);

  /* Pointer into an array literal: walk, read, and write through it. */
  int *p = (int[]){10, 20, 30};
  p++;
  printf("walk %d %d\n", *p, p[1]);
  p[-1] = 77;
  printf("write %d\n", p[-1]);

  /* Degenerate struct base, mutated through the pointer. */
  struct S *q = &(struct S){40, -50};
  q->a += 2;
  printf("ptrstruct %d %d\n", q->a, q->b);

  /* Fresh object per evaluation: the hole write of one iteration must
     not survive into the next iteration's zero fill. */
  int total = 0;
  for (int i = 0; i < 3; i++) {
    int *lp = (int[3]){i + 1};
    total += lp[0] * 100 + lp[1] + lp[2];
    lp[1] = 55; /* dirty a hole; the next evaluation re-zeroes it */
  }
  printf("loop %d\n", total);

  /* A decayed literal passed to a slice parameter. */
  printf("slicearg %d\n", first_two((int[]){61, 3}));

  /* Conditional rebinding across two literals (multi-base region);
     passing a multi-base pointer onward stays rejected (CTS-P7), so the
     reads are direct. */
  for (int c = 0; c < 2; c++) {
    int *rp = (int[]){1, 2};
    if (c)
      rp = (int[]){30, 40};
    printf("rebind %d\n", rp[0] * 10 + rp[1]);
  }

  /* Nested literal as an aggregate element; designated partial literal. */
  struct T t = {(struct S){8, -9}, 10};
  printf("nested %d %d %d\n", t.inner.a, t.inner.b, t.c);
  int *dp = (int[4]){[2] = 5};
  printf("designated %d %d %d %d\n", dp[0], dp[1], dp[2], dp[3]);

  /* String-filled char array literal (NUL fills the tail). */
  char *cs = (char[]){"hi"};
  printf("chars %c%c tail %d\n", cs[0], cs[1], cs[2]);
  return 0;
}

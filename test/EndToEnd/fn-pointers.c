// REQUIRES: cargo
// FR-29: function pointer differential end-to-end test: transpile to a
// cargo crate, build it, and compare its stdout against the natively
// compiled C program. Exercises the 00087/00088 shapes and beyond: an
// enum-keyed dispatch selecting add/sub/mul functions, a struct-member
// function pointer, a null check before an indirect call, a function
// pointer passed to a helper that invokes it, a prototype-less K&R
// pointer, and 00124's nested function-pointer-returning-function shape.
// main returns 0 and reports everything via printf, so lit's per-command
// exit-code checking covers both runs and diff covers the observable
// behavior. Every called pointer is non-null; the program has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/fn_pointers > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }
int mul(int a, int b) { return a * b; }
int zero(void) { return 0; }

enum Op { OP_ADD, OP_SUB, OP_MUL };

typedef int (*binop)(int, int);

/* Enum-keyed dispatch returning a function pointer. */
binop select_op(enum Op op) {
  switch (op) {
  case OP_ADD:
    return add;
  case OP_SUB:
    return sub;
  default:
    return mul;
  }
}

struct Calc {
  int (*op)(int, int);
  int bias;
};

/* A function pointer received by value and null-checked before the call. */
int apply(binop f, int a, int b) {
  if (f)
    return f(a, b);
  return -1;
}

/* 00124 shape: a function returning a function pointer. */
int f2(int c, int b) { return c - b; }

int (*f1(int a, int b))(int c, int d) {
  if (a != b)
    return f2;
  return 0;
}

int main(void) {
  printf("%d\n", select_op(OP_ADD)(7, 5));
  printf("%d\n", select_op(OP_SUB)(7, 5));
  printf("%d\n", select_op(OP_MUL)(7, 5));

  struct Calc c;
  c.op = sub;
  c.bias = 10;
  printf("%d\n", c.op(c.bias, 4));

  binop f = 0;
  printf("%d\n", apply(f, 1, 2));
  f = add;
  printf("%d\n", apply(f, 1, 2));
  if (f != 0)
    printf("%d\n", f(20, 30));

  int (*np)() = zero;
  if (np)
    printf("%d\n", np());

  int (*(*p)(int a, int b))(int c, int d) = f1;
  printf("%d\n", (*(*p)(0, 2))(2, 2));
  if ((*p)(1, 1) == 0)
    printf("%d\n", 42);
  return 0;
}

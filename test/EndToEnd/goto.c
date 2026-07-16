// REQUIRES: cargo
// Differential test for C99-33 goto support across the adversarial CFG
// shapes: a goto jumping over an initialization (the variable place must be
// hoisted so the post-label assignment still dominates), a goto escaping two
// nested loops, a label at the very end of a function, a backward goto that
// forms a loop, and a goto into a loop body (irreducible CFG, structured by
// the lift-cf-to-scf multiplexer). Byte-identical stdout and exit codes
// against the clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/goto > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int skip_init(void) {
  goto skip;
  unsigned u = 9u;
skip:
  u = 3u;
  return (int)u;
}

int escape_nested(int limit) {
  int hits = 0;
  for (int i = 0; i < 10; i = i + 1) {
    for (int j = 0; j < 10; j = j + 1) {
      hits = hits + 1;
      if (hits == limit)
        goto done;
    }
  }
done:
  return hits;
}

void label_at_end(int x) {
  if (x > 2)
    goto out;
  printf("label_at_end: small %d\n", x);
out:;
}

int backward_loop(int n) {
  int acc = 0;
top:
  acc = acc + n;
  n = n - 1;
  if (n > 0)
    goto top;
  return acc;
}

int into_loop(void) {
  int i = 0;
  int n = 0;
  goto inside;
  while (i < 5) {
  inside:
    n = n + 2;
    i = i + 1;
  }
  return n;
}

int chained_labels(void) {
start:
  goto next;
  return 1;
success:
  return 42;
next:
foo:
  goto success;
  return 1;
}

int main(void) {
  printf("skip_init=%d\n", skip_init());
  printf("escape_nested(24)=%d\n", escape_nested(24));
  printf("escape_nested(1000)=%d\n", escape_nested(1000));
  label_at_end(1);
  label_at_end(7);
  printf("backward_loop(5)=%d\n", backward_loop(5));
  printf("into_loop=%d\n", into_loop());
  printf("chained_labels=%d\n", chained_labels());
  return 0;
}

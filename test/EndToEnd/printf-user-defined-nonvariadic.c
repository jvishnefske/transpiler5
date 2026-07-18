// REQUIRES: cargo
// Differential regression test for a USER-DEFINED printf, non-variadic
// shape (the printf-user-defined.c sibling; C allows any signature for a
// project-supplied printf when <stdio.h> is not included). Calls must
// import and call the definition like any ordinary user function instead
// of being intercepted by name into the hosted formatted-print lowering:
// the native build prints CUSTOM lines and returns the definition's own
// result — never the format string. Statement and value positions are
// both exercised; the shadowed program reports through the intercepted
// puts/putchar surface only.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name printf_user_nonvariadic --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/printf_user_nonvariadic > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int puts(const char *);
int putchar(int);

/* Non-variadic user printf: announces itself, echoes its argument, and
   returns a small code. */
static int printf(const char *s) {
  puts("CUSTOM");
  puts(s);
  return 7;
}

int main(void) {
  /* Statement position. */
  printf("stmt-not-a-format\n");

  /* Value position: the result must be the user definition's return. */
  int r = printf("value-not-a-format");
  putchar('0' + r);
  putchar('\n');
  return 0;
}

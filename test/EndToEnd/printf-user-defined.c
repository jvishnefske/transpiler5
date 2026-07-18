// REQUIRES: cargo
// Differential regression test for a USER-DEFINED printf: when the
// project supplies its own internal-linkage printf definition (here the
// variadic, va_list-free shape), calls must reach that definition — like
// any other user function — instead of being intercepted by name and
// lowered to the hosted formatted-print machinery. The body never touches
// va_list, so it imports as its fixed prototype (the named `fmt`
// parameter only) and call-site extras are dropped, which matches C: the
// body cannot observe them. Statement position, value position, and an
// extra-argument call site are all exercised. The program's only true
// output channel is the intercepted puts/putchar surface (printf is
// shadowed), so the native build prints VCUSTOM lines — never the format
// strings. A miscompile that routes these calls to the hosted printf
// prints the format text instead and diverges.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name printf_user_defined --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/printf_user_defined > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int puts(const char *);
int putchar(int);

static int calls;

/* Variadic, va_list-free user printf: announces itself, echoes the format
   through the puts surface, counts the call, and returns a small code. */
static int printf(const char *fmt, ...) {
  puts("VCUSTOM");
  puts(fmt);
  calls++;
  return 3;
}

int main(void) {
  /* Statement position. */
  printf("stmt-should-not-be-formatted\n");

  /* Value position: the result must be the user definition's return. */
  int r = printf("value-should-not-be-formatted %d\n");
  putchar('0' + r);
  putchar('\n');

  /* Extra arguments beyond the fixed prototype are dropped (the body is
     va_list-free and cannot observe them) — same behavior as C. */
  int r2 = printf("extras-dropped\n", 41, 42);
  putchar('0' + r2);
  putchar('\n');

  putchar('0' + calls);
  putchar('\n');
  return 0;
}

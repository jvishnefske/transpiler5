// REQUIRES: cargo
// CTS 00204: differential regression test for the va_list
// monomorphization stack in one piece — a mini-myprintf whose format
// loop dispatches through the keyword-named `match` string-cursor
// helper (const char ** parameter advanced by the callee), va_arg
// reads of three distinct struct types, long-double members printed
// with %.1Lf under the long-double-as-f64 policy, multiple call sites
// of distinct extra signatures (including a zero-extra site), and
// data-dependent field values flowing from a loop counter. All
// floating values are f64-exact (k + small power-of-two fractions), so
// the native x87 long double and the substituted f64 agree byte-exactly
// at one decimal. Byte-identical stdout and exit codes against the
// clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name varargs_monomorph --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/varargs_monomorph > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdarg.h>
#include <stdio.h>

struct d2 {
  double a;
  double b;
};

struct l2 {
  long double a;
  long double b;
};

struct iv {
  int v;
};

int match(const char **s, const char *f) {
  const char *p = *s;
  for (p = *s; *f && *f == *p; f++, p++)
    ;
  if (!*f) {
    *s = p - 1;
    return 1;
  }
  return 0;
}

void myprintf(const char *format, ...) {
  const char *s;
  va_list ap;
  va_start(ap, format);
  for (s = format; *s; s++) {
    if (match(&s, "%d2")) {
      struct d2 x = va_arg(ap, struct d2);
      printf("%.1f,%.1f", x.a, x.b);
    } else if (match(&s, "%l2")) {
      struct l2 x = va_arg(ap, struct l2);
      printf("%.1Lf,%.1Lf", x.a, x.b);
    } else if (match(&s, "%iv")) {
      struct iv x = va_arg(ap, struct iv);
      printf("%d", x.v);
    } else {
      putchar(*s);
    }
  }
  putchar('\n');
}

int main(void) {
  int i;
  for (i = 0; i < 3; i++) {
    struct d2 d;
    struct l2 l;
    struct iv n;
    d.a = i + 0.5;
    d.b = i * 2 + 0.25;
    l.a = i + 10.5;
    l.b = i * 4 + 0.75;
    n.v = i * 7 + 1;
    myprintf("row %iv: %d2 | %l2", n, d, l);
    myprintf("swap %l2 then %d2 and %iv end", l, d, n);
    myprintf("pair %l2 %l2!", l, l);
  }
  myprintf("plain text, no directives");
  return 0;
}

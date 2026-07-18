// RUN: emitrust-import-c %s | FileCheck %s

// A project-supplied printf definition disables the hosted by-name
// printf interception (the same definition guard puts/putchar and the
// <string.h> surface already apply): the definition imports as an
// ordinary function and every call — statement or value position —
// lowers to a plain `func.call @printf`, never to the hosted
// formatted-print lowering.

int puts(const char *);

static int printf(const char *s) {
  puts(s);
  return 7;
}
// CHECK-LABEL: func.func @printf(

int caller(void) {
  printf("x");
  return printf("y");
}
// CHECK-LABEL: func.func @caller
// CHECK: call @printf
// CHECK: call @printf
// CHECK-NOT: print!

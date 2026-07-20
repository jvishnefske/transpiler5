// Companion definition for multi-tu-varargs.c (W3.0). `logf_` is a
// va_list-using variadic DEFINED here; multi-tu-varargs.c calls it from a
// different translation unit, which planVaMonomorph cannot see (it only
// enumerates call sites within its own TU) — a located cross-TU rejection.
//
// This file is ALSO run standalone (multi-tu-varargs.c's SAMETU RUN line)
// as the negative control: the identical variadic, defined AND called
// within this ONE translation unit via `same_tu_use`, must still
// monomorphize (the cross-TU rejection must not over-reject the case
// planVaMonomorph already handles).

#include <stdarg.h>

int logf_(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int x = va_arg(ap, int);
  va_end(ap);
  return x;
}

int same_tu_use(void) { return logf_("%d", 7); }

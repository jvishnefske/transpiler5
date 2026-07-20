// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/hex-conv.c 2>&1 | FileCheck %s --check-prefix=LHEX
// RUN: not emitrust-import-c %t/sizeof-ld.c 2>&1 | FileCheck %s --check-prefix=LDSIZE

// Boundaries of the long-double-as-f64 policy (long-double-f64.c). The
// mapping is a value-level refinement: it holds only while no construct
// observes the difference between the C long double object and the f64
// that stands in for it.

// %La/%LA (hex float output) renders the BITS of the value: an x87
// 80-bit (or f128) native long double and the substituted f64 print
// different hex mantissas even for f64-exact values, so the L-modified
// a/A conversions stay rejected — same policy family as the already
// rejected %a/%A for double.
//--- hex-conv.c
int printf(const char *fmt, ...);
int main(void) {
  printf("%La\n", 1.5L);
  return 0;
}
// LHEX: hex-conv.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported printf format specifier '%La'

// sizeof(long double) folds to the C ABI size (16 on x86-64), but the
// emitted object is an 8-byte f64 — the fold would promise a layout the
// Rust never keeps (the same reasoning as sizeof over bit-field
// structs), so sizeof/_Alignof over long double is a located rejection.
//--- sizeof-ld.c
int main(void) {
  return (int)sizeof(long double);
}
// LDSIZE: sizeof-ld.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: sizeof/alignof of long double

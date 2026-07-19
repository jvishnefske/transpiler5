// C99-8: `long double` is a PERMANENT documented rejection. Rust has no
// extended-precision float type; silently mapping it to f64 would change
// the numeric result and break the byte-exact differential oracle for
// printf %Lf formatting (c-testsuite 00204). The importer rejects with a
// located diagnostic at the first use of the type.
// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

double f(void) {
  long double d = 1.0;
  return (double)d;
}

// CHECK: long-double-invalid.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported builtin type 'long double'

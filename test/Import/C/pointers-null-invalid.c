// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/null-only-deref.c 2>&1 | FileCheck %s --check-prefix=NULLDEREF
// RUN: not emitrust-import-c %t/null-arg.c 2>&1 | FileCheck %s --check-prefix=NULLARG
// RUN: not emitrust-import-c %t/null-ordered.c 2>&1 | FileCheck %s --check-prefix=NULLORD
// RUN: not emitrust-import-c %t/maybe-null-compare.c 2>&1 | FileCheck %s --check-prefix=MAYBECMP
// RUN: not emitrust-import-c %t/maybe-null-arg.c 2>&1 | FileCheck %s --check-prefix=MAYBEARG
// RUN: not emitrust-import-c %t/maybe-null-diff.c 2>&1 | FileCheck %s --check-prefix=MAYBEDIFF
// RUN: not emitrust-import-c %t/null-literal.c 2>&1 | FileCheck %s --check-prefix=NULLLIT

// CTS-P8 boundaries: the Option-of-cursor model covers NULL assignment,
// null-checks, and guarded dereference of pointers with a known base.
// Every null shape outside that — and every construct that would erase a
// pointer's discriminant — stays a located rejection.

// A pointer that only ever holds the null constant has no base object to
// dereference (a null-check on it is fine; see pointers-null.c).
// NULLDEREF: null-only-deref.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: dereference of a pointer that is only ever null

//--- null-only-deref.c
int main(void) {
  int *p = 0;
  return *p;
}

// A null constant passed as a function argument has no modeled consumer;
// the callee's parameter has no null representation.
// NULLARG: null-arg.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null pointer constant in a pointer expression

//--- null-arg.c
int f(int *p) { return p[0]; }
int main(void) {
  return f(0);
}

// C defines only equality against the null constant; ordered comparison
// is UB and stays rejected.
// NULLORD: null-ordered.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: ordered comparison against a null pointer

//--- null-ordered.c
int main(void) {
  int x = 1;
  int *p = &x;
  p = 0;
  if (p > 0)
    return 1;
  return 0;
}

// `p == q` where either side may be null is defined in C (null compares
// unequal to any object address), but the cursor comparison cannot
// express the mixed state.
// MAYBECMP: maybe-null-compare.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: comparison of possibly-null pointers

//--- maybe-null-compare.c
int main(void) {
  int a[4];
  int *p = a;
  int *q = a;
  a[0] = 1;
  p = 0;
  if (p == q)
    return 1;
  return 0;
}

// Passing a possibly-null pointer onward would erase its discriminant
// (and the callee may be a defined C program that null-checks it).
// MAYBEARG: maybe-null-arg.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: possibly-null pointer passed as a function argument

//--- maybe-null-arg.c
int f(int *p) { return p[0]; }
int main(void) {
  int a[4];
  int *p = a;
  a[0] = 9;
  p = 0;
  p = a;
  return f(p);
}

// Pointer difference is defined only for pointers into the same array; a
// possibly-null operand has no defined difference.
// MAYBEDIFF: maybe-null-diff.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: difference of possibly-null pointers

//--- maybe-null-diff.c
int main(void) {
  int a[4];
  int *p = a;
  int *q = a + 2;
  p = 0;
  p = a;
  return q - p;
}

// A nullable string-literal region is outside the CTS-P8 scope.
// NULLLIT: null-literal.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null pointer constant assigned to a pointer into a string literal

//--- null-literal.c
int main(void) {
  char *s = "abc";
  s = 0;
  return 0;
}

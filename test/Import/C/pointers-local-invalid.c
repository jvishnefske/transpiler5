// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/multibase.c 2>&1 | FileCheck %s --check-prefix=MULTI
// RUN: not emitrust-import-c %t/addr-of-ptr.c 2>&1 | FileCheck %s --check-prefix=ADDRPTR
// RUN: not emitrust-import-c %t/ptr-to-ptr.c 2>&1 | FileCheck %s --check-prefix=PTRPTR
// RUN: not emitrust-import-c %t/into-global.c 2>&1 | FileCheck %s --check-prefix=GLOBAL
// RUN: not emitrust-import-c %t/null-init.c 2>&1 | FileCheck %s --check-prefix=NULLP
// RUN: not emitrust-import-c %t/non-address.c 2>&1 | FileCheck %s --check-prefix=NONADDR
// RUN: not emitrust-import-c %t/scalar-arith.c 2>&1 | FileCheck %s --check-prefix=SCALARARITH
// RUN: not emitrust-import-c %t/cross-diff.c 2>&1 | FileCheck %s --check-prefix=CROSSDIFF
// RUN: not emitrust-import-c %t/cross-compare.c 2>&1 | FileCheck %s --check-prefix=CROSSCMP
// RUN: not emitrust-import-c %t/truth-value.c 2>&1 | FileCheck %s --check-prefix=TRUTH
// RUN: not emitrust-import-c %t/string-literal.c 2>&1 | FileCheck %s --check-prefix=STRLIT
// RUN: not emitrust-import-c %t/row-walk.c 2>&1 | FileCheck %s --check-prefix=ROWWALK
// RUN: not emitrust-import-c %t/row-diff.c 2>&1 | FileCheck %s --check-prefix=ROWDIFF

// Phase-1a pointer decomposition boundaries: every pointer local must
// resolve to exactly one non-escaping local object. Each file below
// exercises one located rejection.

// A pointer rebound across two distinct objects (the 00077 shape) cannot
// decompose into a single (base, cursor) pair; the diagnostic names both
// objects and both binding sites.
// MULTI: multibase.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer 'p' would join objects 'x' and 'y' into one region
// MULTI: multibase.c:{{[0-9]+}}:{{[0-9]+}}: note: bound to 'x' here
// MULTI: multibase.c:{{[0-9]+}}:{{[0-9]+}}: note: bound to 'y' here

//--- multibase.c
int main(void) {
  int x[4];
  int y[4];
  int *p;
  x[0] = 1;
  y[0] = 2;
  p = x;
  p = y;
  return p[0];
}

// `&p` would let the pointer itself escape the decomposition.
// ADDRPTR: addr-of-ptr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: taking the address of a pointer variable

//--- addr-of-ptr.c
int main(void) {
  int x = 1;
  int *p = &x;
  int **pp = &p;
  return **pp;
}

// A pointer-to-pointer local has no decomposed representation at all.
// PTRPTR: ptr-to-ptr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-to-pointer variable

//--- ptr-to-ptr.c
int main(void) {
  int **pp;
  return 0;
}

// A pointer into a global would dangle from the staged-copy global access
// model.
// GLOBAL: into-global.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer into a global variable

//--- into-global.c
static int garr[4];

int main(void) {
  int *p = garr;
  return p[0];
}

// Decomposed pointers have no null value.
// NULLP: null-init.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null pointer constant assigned to a pointer variable

//--- null-init.c
int main(void) {
  int *p = 0;
  return 0;
}

// A pointer conjured from a non-address value has no base object.
// NONADDR: non-address.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- non-address.c
int main(void) {
  int x = 64;
  int *p = (int *)x;
  return *p;
}

// The address of a scalar has no elements to walk.
// SCALARARITH: scalar-arith.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: arithmetic on the address of a scalar object

//--- scalar-arith.c
int main(void) {
  int x = 1;
  int *p = &x;
  p = p + 1;
  return *p;
}

// Pointers into different objects have no defined difference in C.
// CROSSDIFF: cross-diff.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: difference of pointers into different objects

//--- cross-diff.c
int main(void) {
  int a[2];
  int b[2];
  int *q = a;
  int *r = b;
  return (int)(q - r);
}

// Pointers into different objects have no defined ordering in C.
// CROSSCMP: cross-compare.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: comparison of pointers into different objects

//--- cross-compare.c
int main(void) {
  int a[2];
  int b[2];
  int *q = a;
  int *r = b;
  if (q < r) {
    return 1;
  }
  return 0;
}

// A pointer truth test is a null check, and decomposed pointers are never
// null.
// TRUTH: truth-value.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer used as a truth value

//--- truth-value.c
int main(void) {
  int x = 1;
  int *p = &x;
  if (p) {
    return 1;
  }
  return 0;
}

// String literals live in static storage the decomposition cannot own.
// STRLIT: string-literal.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer to a string literal

//--- string-literal.c
int main(void) {
  char *s = "hi";
  return s[0];
}

// Walking a row pointer (`char (*)[4]`) would need a row-scaled cursor
// step; the flat row-major cursor only implements the subscript and
// address-of forms (CTS-P scope).
// ROWWALK: row-walk.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: arithmetic on a pointer to an array

//--- row-walk.c
int main(void) {
  char arr[2][4];
  char (*p)[4];
  p = arr;
  arr[0][0] = 1;
  p++;
  return p[0][0];
}

// A difference of row pointers would need a row-scaled division of the
// flat cursors (CTS-P scope).
// ROWDIFF: row-diff.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: arithmetic on a pointer to an array

//--- row-diff.c
int main(void) {
  char arr[2][4];
  char (*p)[4];
  char (*r)[4];
  p = arr;
  r = arr;
  arr[0][0] = 1;
  return (int)(p - r);
}

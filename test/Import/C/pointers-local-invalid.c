// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/multibase.c 2>&1 | FileCheck %s --check-prefix=MULTI
// RUN: not emitrust-import-c %t/addr-of-ptr.c 2>&1 | FileCheck %s --check-prefix=ADDRPTR
// RUN: not emitrust-import-c %t/ptr-to-ptr.c 2>&1 | FileCheck %s --check-prefix=PTRPTR
// RUN: not emitrust-import-c %t/into-global.c 2>&1 | FileCheck %s --check-prefix=GLOBAL
// RUN: not emitrust-import-c %t/null-arg.c 2>&1 | FileCheck %s --check-prefix=NULLP
// RUN: not emitrust-import-c %t/non-address.c 2>&1 | FileCheck %s --check-prefix=NONADDR
// RUN: not emitrust-import-c %t/scalar-arith.c 2>&1 | FileCheck %s --check-prefix=SCALARARITH
// RUN: not emitrust-import-c %t/cross-diff.c 2>&1 | FileCheck %s --check-prefix=CROSSDIFF
// RUN: not emitrust-import-c %t/cross-compare.c 2>&1 | FileCheck %s --check-prefix=CROSSCMP
// RUN: not emitrust-import-c %t/row-walk.c 2>&1 | FileCheck %s --check-prefix=ROWWALK
// RUN: not emitrust-import-c %t/row-diff.c 2>&1 | FileCheck %s --check-prefix=ROWDIFF
// RUN: not emitrust-import-c %t/string-literal-write.c 2>&1 | FileCheck %s --check-prefix=STRWRITE
// RUN: not emitrust-import-c %t/string-literal-write-deref.c 2>&1 | FileCheck %s --check-prefix=STRWRITEDEREF
// RUN: not emitrust-import-c %t/string-literal-multi.c 2>&1 | FileCheck %s --check-prefix=STRMULTI
// RUN: not emitrust-import-c %t/string-literal-join.c 2>&1 | FileCheck %s --check-prefix=STRJOIN

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

// A local pointer into a global aggregate is supported (CTS-P6) for
// direct reads and writes through the staged-copy model, but passing it
// to a function stays rejected: the argument would borrow the staged
// local copy, not the global itself, so a callee that also touches the
// global would observe (or lose) the wrong values — the staged-copy
// coherence hazard.
// GLOBAL: into-global.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer into a global variable to a function

//--- into-global.c
static int garr[4];

static int first(int *p) { return p[0]; }

int main(void) {
  int *p = garr;
  return first(p);
}

// A null pointer constant is modeled only where the CTS-P8
// Option-of-cursor discrimination applies (pointer assignment, equality
// comparison, truth test; see pointers-null.c and
// pointers-null-invalid.c); one used as a call argument has no modeled
// consumer and stays rejected.
// NULLP: null-arg.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null pointer constant in a pointer expression

//--- null-arg.c
int f(int *p) { return p[0]; }
int main(void) {
  return f(0);
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

// A string-literal region is read-only (writing a C string literal is
// UB); any write through its pointers is rejected at the write site.
// STRWRITE: string-literal-write.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: write through a pointer to a string literal (the literal is read-only)

//--- string-literal-write.c
int main(void) {
  char *s = "hi";
  s[0] = 'H';
  return s[0];
}

// The same rejection covers a write through a copied cursor (`*q = ...`
// after `q = s`), which shares the literal's region.
// STRWRITEDEREF: string-literal-write-deref.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: write through a pointer to a string literal (the literal is read-only)

//--- string-literal-write-deref.c
int main(void) {
  char *s = "hi";
  char *q = s;
  *q++ = 'H';
  return s[0];
}

// Rebinding one pointer across two distinct literals would need a
// multi-base region (CTS-P7 territory) and stays rejected.
// STRMULTI: string-literal-multi.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer bound to multiple string literals

//--- string-literal-multi.c
int main(void) {
  char *s = "hi";
  s = "bye";
  return s[0];
}

// A pointer cannot range over both a string literal and an object; the
// diagnostic names both bindings.
// STRJOIN: string-literal-join.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer 's' would join a string literal and object 'buf' into one region
// STRJOIN: string-literal-join.c:{{[0-9]+}}:{{[0-9]+}}: note: bound to a string literal here
// STRJOIN: string-literal-join.c:{{[0-9]+}}:{{[0-9]+}}: note: bound to 'buf' here

//--- string-literal-join.c
int main(void) {
  char buf[4];
  char *s = "hi";
  s = buf;
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

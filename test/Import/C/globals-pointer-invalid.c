// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/local-bind.c 2>&1 | FileCheck %s --check-prefix=LOCALBIND
// RUN: not emitrust-import-c %t/multi-object.c 2>&1 | FileCheck %s --check-prefix=MULTIOBJ
// RUN: not emitrust-import-c %t/copy-to-local.c 2>&1 | FileCheck %s --check-prefix=COPY
// RUN: not emitrust-import-c %t/addr-of-global-ptr.c 2>&1 | FileCheck %s --check-prefix=ADDROF
// RUN: not emitrust-import-c %t/pass-to-fn.c 2>&1 | FileCheck %s --check-prefix=PASSFN
// RUN: not emitrust-import-c %t/unbound.c 2>&1 | FileCheck %s --check-prefix=UNBOUND
// RUN: not emitrust-import-c %t/null-init.c 2>&1 | FileCheck %s --check-prefix=NULLINIT
// RUN: not emitrust-import-c %t/string-literal.c 2>&1 | FileCheck %s --check-prefix=STRLIT
// RUN: not emitrust-import-c %t/multi-alloc.c 2>&1 | FileCheck %s --check-prefix=MULTIALLOC
// RUN: not emitrust-import-c %t/static-local.c 2>&1 | FileCheck %s --check-prefix=STATICLOCAL
// RUN: not emitrust-import-c %t/string-write.c 2>&1 | FileCheck %s --check-prefix=STRWRITE
// RUN: not emitrust-import-c %t/string-null.c 2>&1 | FileCheck %s --check-prefix=STRNULL
// RUN: not emitrust-import-c %t/string-wide.c 2>&1 | FileCheck %s --check-prefix=STRWIDE
// RUN: not emitrust-import-c %t/string-nonascii.c 2>&1 | FileCheck %s --check-prefix=STRNONASCII
// RUN: not emitrust-import-c %t/string-object-join.c 2>&1 | FileCheck %s --check-prefix=STROBJ

// CTS-P4 boundaries: every pointer-typed global must resolve to exactly
// one global region base. Each file below exercises one located rejection.

// The borrow-escape case: a global pointer bound to a local object would
// store a borrow that outlives the object's scope — the exact program
// rustc refuses. Rejected at the binding site.
// LOCALBIND: local-bind.c:5:{{[0-9]+}}: error: unsupported: global pointer bound to local object 'x' (the borrow would outlive the object)

//--- local-bind.c
int *g;

void f(void) {
  int x;
  g = &x;
}

int rd(void) { return *g; }
int main(void) { return 0; }

// A global pointer rebound across two distinct global objects cannot
// decompose into a single (base, cursor) pair; the diagnostic names both
// objects and both binding sites.
// MULTIOBJ: multi-object.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global pointer 'g' would join objects '{{[ab]}}' and '{{[ab]}}' into one region
// MULTIOBJ: note: bound to '{{[ab]}}' here
// MULTIOBJ: note: bound to '{{[ab]}}' here

//--- multi-object.c
int a[2];
int b[2];
int *g = a;

void f(void) { g = b; }
int rd(void) { return *g; }
int main(void) { return 0; }

// Copying a global pointer would couple a local cursor cell to the stored
// global cursor; rejected at the copy.
// COPY: copy-to-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: copying a global pointer variable

//--- copy-to-local.c
int a[2];
int *g = a;

int rd(void) {
  int *p;
  p = g;
  return *p;
}

int main(void) { return 0; }

// `&g` would let the pointer escape the decomposition.
// ADDROF: addr-of-global-ptr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: taking the address of a pointer variable

//--- addr-of-global-ptr.c
int a[2];
int *g = a;

int rd(void) { return *g; }

void esc(void) {
  int **pp = &g;
}

int main(void) { return 0; }

// Passing a global-based pointer to a function would pass a borrow of the
// staged local copy, not of the global itself.
// PASSFN: pass-to-fn.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer into a global variable to a function

//--- pass-to-fn.c
int a[2];
int *g = a;

int sum(int *p) { return p[0]; }
int rd(void) { return sum(g); }
int main(void) { return 0; }

// A referenced global pointer that is never bound anywhere has no region.
// UNBOUND: unbound.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global pointer variable 'g' has no known target object

//--- unbound.c
int *g;

int rd(void) { return *g; }
int main(void) { return 0; }

// Decomposed pointers have no null value (CTS-P8 scope).
// NULLINIT: null-init.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null pointer constant assigned to a pointer variable

//--- null-init.c
int *g = 0;

int rd(void) { return *g; }
int main(void) { return 0; }

// A file-scope string-literal *initializer* is supported (CTS-L3), but a
// *body* binding to a literal stays rejected: only the constant
// initializer path synthesizes the module-level read-only backing.
// STRLIT: string-literal.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global pointer bound to a string literal

//--- string-literal.c
char *g;

void f(void) { g = "hi"; }
int rd(void) { return *g; }
int main(void) { return 0; }

// Two distinct allocation sites cannot share one backing array.
// MULTIALLOC: multi-alloc.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global pointer bound to multiple allocations

//--- multi-alloc.c
#include <stdlib.h>
int *g;

void f1(void) { g = (int *)malloc(8); }
void f2(void) { g = (int *)malloc(12); }
int rd(void) { return *g; }
int main(void) { return 0; }

// Pointer-typed function-local statics keep the historical rejection: the
// global pointer model only covers file-scope variables.
// STATICLOCAL: static-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-typed global variable

//--- static-local.c
int rd(void) {
  static int *sp;
  return 0;
}

int main(void) { return 0; }

// A literal-initialized global pointer's region is read-only (writing a C
// string literal is UB); any write through it rejects at the write, even
// when the write is the pointer's only body mention (CTS-L3).
// STRWRITE: string-write.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: write through a pointer to a string literal (the literal is read-only)

//--- string-write.c
char *g = "hi";

void f(void) { *g = 'x'; }
int main(void) { return 0; }

// Nullable literal regions are outside the CTS-P8 scope, matching the
// function-local literal-region policy.
// STRNULL: string-null.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null pointer constant assigned to a pointer into a string literal

//--- string-null.c
char *g = "hi";

void f(void) { g = 0; }
int rd(void) { return *g; }
int main(void) { return 0; }

// Only ordinary (byte) literals have the module-level backing shape; a
// wide literal bound to a pointer stays rejected.
// STRWIDE: string-wide.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-ordinary string literal bound to a pointer

//--- string-wide.c
int *g = L"hi";
int main(void) { return g[0]; }

// The literal backing keeps the C99-28 ASCII policy at file scope, like
// the CTS-P1 function-local backing.
// STRNONASCII: string-nonascii.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-ASCII byte in string literal bound to a pointer

//--- string-nonascii.c
char *g = "caf\xff";
int main(void) { return g[0]; }

// A literal initializer and a body binding to a real object are two
// region base kinds; the pointer cannot range over both.
// STROBJ: string-object-join.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global pointer 'g' bound to multiple objects

//--- string-object-join.c
char c;
char *g = "hi";

void f(void) { g = &c; }
int rd(void) { return *g; }
int main(void) { return 0; }

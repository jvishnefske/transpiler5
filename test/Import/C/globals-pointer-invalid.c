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

// A string-literal backing is function-local today (CTS-L3 scope); a
// global cursor into it has no representation.
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

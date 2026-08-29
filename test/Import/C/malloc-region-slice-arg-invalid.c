// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/alias-same.c 2>&1 | FileCheck %s --check-prefix=SAME
// RUN: not emitrust-import-c %t/alias-offset.c 2>&1 | FileCheck %s --check-prefix=OFFSET
// RUN: not emitrust-import-c %t/alias-copy.c 2>&1 | FileCheck %s --check-prefix=COPY
// RUN: not emitrust-import-c %t/alias-mixed.c 2>&1 | FileCheck %s --check-prefix=MIXED
// RUN: not emitrust-import-c %t/alias-named.c 2>&1 | FileCheck %s --check-prefix=NAMED
// RUN: not emitrust-import-c %t/indirect.c 2>&1 | FileCheck %s --check-prefix=INDIRECT
// RUN: not emitrust-import-c %t/return-ptr.c 2>&1 | FileCheck %s --check-prefix=RETPTR
// RUN: emitrust-cc --emit=import --recover %t/alias-same.c -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=SAMEREC

// FR-147's LOAD-BEARING guard. FR-147 admits an allocation-backed pointer
// as a slice argument; this file pins what stops that from becoming an
// E0499/E0502 generator. `emitCall`'s historical aliasing guard is keyed on
// the argument's `VarDecl` ROOT, and a heap region HAS NO root — the key is
// null, the whole check is skipped, and two arguments into ONE allocation
// would emit two simultaneous borrows of one backing array. That failure
// shows up only as a rustc borrowck error in the emitted crate: a broken
// crate, not a diagnostic, and `emitrust-import-c` would call it a success.
// So FR-147 gives the guard a SECOND, backing-keyed identity (one backing
// per allocation region since FR-146) and every overlapping pair below
// rejects LOCATED.
//
// A backing borrow runs OPEN-ENDED from its cursor to the end of the
// allocation, so ANY two borrows of one backing overlap: `g(p, p)`,
// `g(p, p + 1)` and `g(p, q)` after `q = p` are all rejections, not
// "correct code". Distinct allocations and a named root alongside a heap
// region are ADMITTED instead — those live in malloc-region-slice-arg.c.
//
// Every wording here is also a NULL-ROOT SAFETY pin. FR-146 was a SEGFAULT
// caused by dereferencing a null `VarDecl` WHILE FORMATTING a rejection
// message, and the historical aliasing diagnostic one line away from this
// new one formats `root->getName()`. The heap wording therefore names the
// ALLOCATION and no decl at all; if a future change routes a heap borrow
// into the named wording, `getName()` on a null root crashes and every
// FileCheck in this file fails at once.

// Both arguments are the SAME pointer.
//--- alias-same.c
#include <stdlib.h>
static int g(const char *a, const char *b) { return a[0] + b[0]; }
int f(void) {
  char *p = (char *)malloc(8);
  p[0] = 3;
  return g(p, p);
}
// SAME: alias-same.c:6:10: error: unsupported: aliasing mutable pointer arguments (two arguments borrow the same heap allocation)
// SAME-NOT: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object

// OFFSET arguments into one allocation. The two windows are disjoint in C
// but the emitted borrows are not (each runs to the end of the backing),
// so the pair is refused rather than narrowed.
//--- alias-offset.c
#include <stdlib.h>
static int g(const char *a, const char *b) { return a[0] + b[0]; }
int f(void) {
  char *p = (char *)malloc(8);
  p[0] = 3;
  return g(p, p + 1);
}
// OFFSET: alias-offset.c:6:10: error: unsupported: aliasing mutable pointer arguments (two arguments borrow the same heap allocation)

// A COPIED pointer. `q = p` unites q into p's allocation region, so both
// decompose against the SAME backing place — the key is the backing, not
// the pointer variable, precisely so this shape cannot slip through as two
// different names.
//--- alias-copy.c
#include <stdlib.h>
static int g(const char *a, const char *b) { return a[0] + b[0]; }
int f(void) {
  char *p = (char *)malloc(8);
  char *q = p;
  p[0] = 3;
  return g(p, q);
}
// COPY: alias-copy.c:7:10: error: unsupported: aliasing mutable pointer arguments (two arguments borrow the same heap allocation)

// A named root and a heap region TOGETHER, with the heap region repeated.
// The named key admits `w`, and the backing key still catches `p` twice —
// the two key spaces are checked independently and neither masks the other.
// The diagnostic is the HEAP one, so no decl name is formatted for the
// root-less borrow.
//--- alias-mixed.c
#include <stdlib.h>
static int g(const char *a, const char *b, const char *c) {
  return a[0] + b[0] + c[0];
}
int f(void) {
  char w[8];
  char *p = (char *)malloc(8);
  w[0] = 1;
  p[0] = 2;
  return g(w, p, p);
}
// MIXED: alias-mixed.c:10:10: error: unsupported: aliasing mutable pointer arguments (two arguments borrow the same heap allocation)
// MIXED-NOT: borrow object

// REGRESSION GUARD for the NAMED-root path, which FR-147 must not shift.
// The historical key (root + member path) and its exact wording are pinned
// by pointers-param-invalid.c and friends; this repeats the check right
// next to the new one so a refactor that unified the two keys — or that
// re-keyed a heap borrow onto the pointer's own `VarDecl`, the tempting
// and WRONG shortcut — is caught here as well.
//--- alias-named.c
static int g(int *a, int *b) { return a[0] + b[1]; }
int f(void) {
  int arr[64];
  arr[0] = 1;
  arr[1] = 2;
  return g(arr, arr);
}
// NAMED: alias-named.c:6:10: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 'arr')

// An INDIRECT (function-pointer) call has its own argument loop and does
// NOT key on the backing, so it keeps FR-146's rejection verbatim.
// Refusing is safe; admitting an unkeyed borrow is not.
//--- indirect.c
#include <stdlib.h>
static int g(const char *b) { return b[0]; }
int f(int n) {
  int (*fp)(const char *) = g;
  char *p = (char *)malloc(8);
  p[0] = 3;
  return fp(p) + n;
}
// INDIRECT: indirect.c:7:10: error: unsupported: passing a pointer into a heap allocation as a slice argument

// A callee that RETURNS a pointer into its slice parameter (FR-104). The
// re-slice needs the argument's caller-local region base to re-index, and
// a heap region has none — `root` stays null by construction here, so the
// capture is never filled and the arm rejects LOCATED. Getting this wrong
// is a miscompile, not a build failure: an unfilled capture that fell
// through would bind the result at cursor 0 and silently drop the call.
//--- return-ptr.c
#include <stdlib.h>
static const char *walk(const char *b) { return b + 1; }
int f(void) {
  char *p = (char *)malloc(8);
  p[0] = 3;
  p[1] = 9;
  const char *z = walk(p);
  return z[0];
}
// RETPTR: return-ptr.c:7:19: error: unsupported: returned pointer value (the rooted region argument is not a caller-local region)

// Recovery mode must not launder the aliasing pair into emitted code: the
// function is dropped with the SAME located diagnostic, never lowered.
// SAMEREC: alias-same.c:6:10: warning: unsupported: aliasing mutable pointer arguments (two arguments borrow the same heap allocation) (recovered: emitted an unimplemented!() stub with the mapped signature)
// SAMEREC: emitrust.call_opaque "unimplemented!"() {args = ["unsupported: aliasing mutable pointer arguments (two arguments borrow the same heap allocation)"]}
// SAMEREC-NOT: emitrust.slice_of

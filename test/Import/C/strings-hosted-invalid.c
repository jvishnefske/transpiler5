// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/strstr-call.c 2>&1 | FileCheck %s --check-prefix=STRSTR
// RUN: not emitrust-import-c %t/strcpy-value.c 2>&1 | FileCheck %s --check-prefix=STRCPYVALUE
// RUN: not emitrust-import-c %t/strcpy-alias.c 2>&1 | FileCheck %s --check-prefix=STRCPYALIAS
// RUN: not emitrust-import-c %t/strcpy-into-literal.c 2>&1 | FileCheck %s --check-prefix=STRCPYLIT
// RUN: not emitrust-import-c %t/strchr-value.c 2>&1 | FileCheck %s --check-prefix=STRCHRVALUE
// RUN: not emitrust-import-c %t/strchr-bind.c 2>&1 | FileCheck %s --check-prefix=STRCHRBIND

// C99-48 / CTS-L1 boundaries: <string.h> shapes outside the hosted subset
// keep located rejections.

// A <string.h> function outside the curated table (strstr here) keeps the
// system-header rejection.
//--- strstr-call.c
#include <string.h>
int main(void) {
  char a[8] = "abc";
  strstr(a, "b");
  return 0;
}
// STRSTR: strstr-call.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to 'strstr' declared in a system header

// C's strcpy returns the destination pointer, which has no decomposed
// representation; the copy functions are statement-position only. (With
// the standard char* prototype the pointer-region analysis rejects the
// binding first; the K&R-style int declaration reaches the call check.)
//--- strcpy-value.c
int strcpy(char *, char *);
int main(void) {
  char a[8];
  char b[8];
  b[0] = 0;
  int r = strcpy(a, b);
  return r;
}
// STRCPYVALUE: strcpy-value.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: strcpy return value must be unused

// Source and destination rooted in the same object would alias the
// mutable destination borrow.
//--- strcpy-alias.c
#include <string.h>
int main(void) {
  char a[8] = "ab";
  strcpy(a, &a[1]);
  return 0;
}
// STRCPYALIAS: strcpy-alias.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: strcpy source and destination point into the same object 'a'

// A string literal region is read-only; it cannot be a copy destination.
//--- strcpy-into-literal.c
#include <string.h>
int main(void) {
  char a[8] = "ab";
  strcpy("xy", a);
  return 0;
}
// STRCPYLIT: strcpy-into-literal.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a string literal region cannot be a mutable string argument

// strchr results are consumed by printf %s and null comparisons only;
// every other value use keeps a located rejection.
//--- strchr-value.c
#include <string.h>
int main(void) {
  char a[8] = "ab";
  strchr(a, 'a');
  return 0;
}
// STRCHRVALUE: strchr-value.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a strchr result must feed a printf '%s' argument or a comparison against a null pointer

// Binding the strchr result to a pointer local is equally outside the
// accepted consumption shapes (printf %s argument or null comparison).
// The pointer-region analysis rejects the initializer before the
// strchr-specific check is reached — a strchr call is not an address
// expression a decomposed pointer can be assigned from — so this shape
// pins the earlier diagnostic; the strchr-specific wording above stays
// pinned by strchr-value.c.
//--- strchr-bind.c
#include <string.h>
int main(void) {
  char a[8] = "ab";
  char *p = strchr(a, 'a');
  return p == 0;
}
// STRCHRBIND: strchr-bind.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

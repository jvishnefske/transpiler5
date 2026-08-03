// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/keyword-collision.c 2>&1 | FileCheck %s --check-prefix=COLLIDE
// RUN: not emitrust-import-c %t/cursor-global-store.c 2>&1 | FileCheck %s --check-prefix=STORE
// RUN: not emitrust-import-c %t/cursor-pass-on.c 2>&1 | FileCheck %s --check-prefix=PASSON
// RUN: not emitrust-import-c %t/cursor-triple.c 2>&1 | FileCheck %s --check-prefix=TRIPLE
// RUN: not emitrust-import-c %t/cursor-void.c 2>&1 | FileCheck %s --check-prefix=VOIDPP

// Boundaries of the keyword-fn mangle and the T** cursor parameter
// (keyword-fn-and-cursor.c, pointers-cursor-param-general.c). C99-43
// slice 1a generalized the cursor eligibility from char** to any
// two-level data pointer with a slice-valid element; the escape
// rejections keep their located diagnostics under the generalized
// wording ("escapes the cursor-parameter shape", ledger tag
// ptr-to-ptr-shape-escape), while T*** and void** stay OUTSIDE the
// eligible shape entirely and keep the historical generic
// pointer-to-pointer rejection at mapParamType.

// The trailing-underscore mangle must not silently merge two C symbols:
// when the source already declares the mangled spelling, the keyword
// function is rejected where it is declared.
//--- keyword-collision.c
int match_(int y) { return y + 1; }
int match(int x) { return x; }
int main(void) {
  return match(1) + match_(2);
}
// COLLIDE: keyword-collision.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function name 'match' mangles to 'match_', which collides with an existing symbol

// A const char ** parameter is only the bounded string cursor: read the
// current position, advance it. Storing the parameter itself into a
// global escapes the cursor.
//--- cursor-global-store.c
const char **stash;
int keep(const char **s, const char *f) {
  stash = s;
  return 0;
}
int main(void) {
  const char *t = "ab";
  return keep(&t, "a");
}
// STORE: cursor-global-store.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-to-pointer parameter escapes the cursor-parameter shape

// Forwarding the cursor parameter to another function likewise leaves
// the bounded shape.
//--- cursor-pass-on.c
int inner(const char **s) { return **s == 'a'; }
int hop(const char **s, const char *f) { return inner(s); }
int main(void) {
  const char *t = "ab";
  return hop(&t, "a");
}
// PASSON: cursor-pass-on.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-to-pointer parameter escapes the cursor-parameter shape

// Three pointer levels have no cursor decomposition: T*** keeps the
// historical generic rejection (the element of a T*** cursor would
// itself be a pointer, which no slice models).
//--- cursor-triple.c
int deep(int ***p) { return ***p; }
int main(void) {
  int x = 1;
  int *px = &x;
  int **ppx = &px;
  return deep(&ppx);
}
// TRIPLE: cursor-triple.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-to-pointer parameter

// void** names no element type to slice over; unchanged wording.
//--- cursor-void.c
int peek(void **p) { return *p != 0; }
int main(void) {
  void *v = 0;
  return peek(&v);
}
// VOIDPP: cursor-void.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-to-pointer parameter

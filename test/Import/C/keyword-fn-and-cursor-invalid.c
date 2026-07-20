// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/keyword-collision.c 2>&1 | FileCheck %s --check-prefix=COLLIDE
// RUN: not emitrust-import-c %t/cursor-global-store.c 2>&1 | FileCheck %s --check-prefix=STORE
// RUN: not emitrust-import-c %t/cursor-pass-on.c 2>&1 | FileCheck %s --check-prefix=PASSON

// Boundaries of the keyword-fn mangle and the const char ** string
// cursor (keyword-fn-and-cursor.c).

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
// STORE: cursor-global-store.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-to-pointer parameter escapes the string-cursor shape

// Forwarding the cursor parameter to another function likewise leaves
// the bounded shape.
//--- cursor-pass-on.c
int inner(const char **s) { return **s == 'a'; }
int hop(const char **s, const char *f) { return inner(s); }
int main(void) {
  const char *t = "ab";
  return hop(&t, "a");
}
// PASSON: cursor-pass-on.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-to-pointer parameter escapes the string-cursor shape

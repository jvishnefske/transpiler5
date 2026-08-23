// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/multi-param.c 2>&1 | FileCheck %s --check-prefix=MULTIPARAM
// RUN: not emitrust-import-c %t/global-table.c 2>&1 | FileCheck %s --check-prefix=GLOBALTBL
// RUN: not emitrust-import-c %t/param-local-mix.c 2>&1 | FileCheck %s --check-prefix=MIXED
// RUN: not emitrust-import-c %t/heap-decl.c 2>&1 | FileCheck %s --check-prefix=HEAPDECL
// RUN: not emitrust-import-c %t/literal-arg.c 2>&1 | FileCheck %s --check-prefix=LITERALARG

// FR-104 frontier: the parameter-cursor return transform admits ONLY
// functions whose EVERY return site roots in the SAME single pointer
// parameter. Everything outside that decidable sub-family keeps its
// verbatim located rejection — return sites rooting in two different
// parameters (no single region for the caller to re-slice), a global
// table return (log_level_string family; the cursor would cross into a
// region the caller never passed), a parameter/callee-local mix (the
// local site would dangle), and a body-less heap-returning declaration
// (classification is definition-body-driven). Rejection is a feature:
// none of these may silently emit wrong code.

//--- multi-param.c
// Return sites root in TWO different parameters: multi-base, rejected at
// the first offending return site.
const char *pick(const char *a, const char *b, int w) {
  if (w) return a;
  return b;
}
int main(void) {
  char x[2] = "x", y[2] = "y";
  return *pick(x, y, 1) == 'x' ? 0 : 1;
}
// MULTIPARAM: multi-param.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: returned pointer value (only a returned function address has a representation; a cursor into a callee-local region would dangle)

//--- global-table.c
// A cursor into a STATIC TABLE is not a cursor into a caller-supplied
// region: the caller holds no argument region to re-slice.
static char tbl[4] = "abc";
char *first(void) { return tbl; }
int main(void) { return *first() == 'a' ? 0 : 1; }
// GLOBALTBL: global-table.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: returned pointer value (only a returned function address has a representation; a cursor into a callee-local region would dangle)

//--- param-local-mix.c
// One site roots in the parameter, one in a callee local: the local site
// would dangle, so the whole function stays out (all-or-nothing proof).
int *pick(int *a, int w) {
  int loc = 9;
  if (w) return a;
  return &loc;
}
int main(void) {
  int arr[2] = {4, 5};
  return *pick(arr, 1) - 4;
}
// MIXED: param-local-mix.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: returned pointer value (only a returned function address has a representation; a cursor into a callee-local region would dangle)

//--- heap-decl.c
// A body-less pointer-returning declaration has no return sites to
// classify (the cross-TU cliff): the historical signature rejection.
void *malloc(unsigned long);
static int *make(void) {
  int *p = (int *)malloc(sizeof(int));
  *p = 5;
  return p;
}
int main(void) { return *make() - 5; }
// HEAPDECL: heap-decl.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer return type

//--- literal-arg.c
// Caller-side frontier: the transform's re-slice needs a caller-LOCAL
// single-object region behind the rooted argument. A string-literal
// argument resolves to a base-less read-only backing with no whole-region
// place the returned cursor could re-index, so the call rejects located
// (prefixed "returned pointer value" so the rejection ledger keeps it in
// the returned-pointer family).
char *lskip(const char *s) {
  while (*s && *s <= ' ')
    s++;
  return (char *)s;
}
int main(void) { return *lskip("  a") == 'a' ? 0 : 1; }
// LITERALARG: literal-arg.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: returned pointer value (the rooted region argument is not a caller-local region)

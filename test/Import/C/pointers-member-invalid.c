// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/conflict.c 2>&1 | FileCheck %s --check-prefix=CONFLICT
// RUN: not emitrust-import-c %t/unbound.c 2>&1 | FileCheck %s --check-prefix=UNBOUND
// RUN: not emitrust-import-c %t/aliased-write.c 2>&1 | FileCheck %s --check-prefix=ALIASED
// RUN: not emitrust-import-c %t/struct-overwrite.c 2>&1 | FileCheck %s --check-prefix=OVERWRITE
// RUN: not emitrust-import-c %t/literal-read.c 2>&1 | FileCheck %s --check-prefix=LITREAD
// RUN: not emitrust-import-c %t/array-bound.c 2>&1 | FileCheck %s --check-prefix=ARRAY
// RUN: not emitrust-import-c %t/cross-function-local.c 2>&1 | FileCheck %s --check-prefix=CROSSFN
// RUN: not emitrust-import-c %t/member-escape.c 2>&1 | FileCheck %s --check-prefix=ESCAPE
// RUN: not emitrust-import-c %t/reinterpret-cast.c 2>&1 | FileCheck %s --check-prefix=REINTERP

// CTS-P2 boundaries of the pointer-struct-member model: a data-pointer
// member must resolve to one statically known target per struct instance,
// and every unresolvable shape is a located rejection. Each file below
// pins one boundary.

//--- conflict.c
// Two bindings of one member to different objects clash; the diagnostic
// names the member and both sites.
struct S { int *p; };
int main(void) {
  struct S s;
  int a = 1, b = 2;
  s.p = &a;
  s.p = &b;
  return *s.p;
}
// CONFLICT: conflict.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member 'p' bound to two different targets
// CONFLICT: note: first bound here
// CONFLICT: note: conflicting binding here

//--- unbound.c
// Reading a member that was never bound (C: a null/indeterminate
// dereference) rejects at the read.
struct S { int *p; };
int main(void) {
  struct S s;
  return *s.p;
}
// UNBOUND: unbound.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member 'p' has no known target object

//--- aliased-write.c
// A member write through an alias (`q->p = ...`) has an instance path the
// per-instance model cannot key; the field is poisoned program-wide and
// the read names the unresolvable site.
struct S { int *p; };
void bind(struct S *q, int *v) { q->p = v; }
int main(void) {
  struct S s;
  int a = 1;
  s.p = &a;
  bind(&s, &a);
  return *s.p;
}
// ALIASED: aliased-write.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member 'p' is used outside the static-binding model
// ALIASED: note: first unresolvable use is here

//--- struct-overwrite.c
// A whole-struct assignment replaces the member wholesale; no static
// binding survives it.
struct S { int *p; };
int main(void) {
  struct S s, t;
  int a = 1;
  s.p = &a;
  t = s;
  return *t.p;
}
// OVERWRITE: struct-overwrite.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member 'p' is used outside the static-binding model

//--- literal-read.c
// A literal-bound member is write-only: nothing in the supported subset
// reads it back (the backing is never materialized).
struct W { char *data; };
int main(void) {
  struct W w = { "bugs" };
  return *w.data;
}
// LITREAD: literal-read.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member 'data' bound to a string literal

//--- array-bound.c
// A member bound to an array element run would need runtime cursor state;
// the degenerate member model rejects it at the binding.
struct S { int *p; };
int main(void) {
  struct S s;
  int arr[3];
  s.p = arr;
  return 0;
}
// ARRAY: array-bound.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member bound to an array

//--- cross-function-local.c
// A global instance's member bound to a local of another function would
// dangle at the read; the read rejects.
struct S { int *p; };
struct S gs;
void bind(void) {
  int local = 1;
  gs.p = &local;
}
int main(void) {
  bind();
  return *gs.p;
}
// CROSSFN: cross-function-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member 'p' bound to a local object of another function

//--- member-escape.c
// Taking the member's own address lets it escape the static model.
struct S { int *p; };
int main(void) {
  struct S s;
  int a = 1;
  s.p = &a;
  int **pp = &s.p;
  return *s.p;
}
// ESCAPE: member-escape.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member 'p' is used outside the static-binding model

//--- reinterpret-cast.c
// Only qualification-preserving casts peel; a cast that changes the
// pointee reinterprets the decomposition's element unit and stays
// rejected.
int main(void) {
  long x = 1;
  char *c = (char *)&x;
  return *c;
}
// REINTERP: reinterpret-cast.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

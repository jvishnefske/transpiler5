// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/same-field.c 2>&1 | FileCheck %s --check-prefix=SAMEFIELD
// RUN: not emitrust-import-c %t/whole-plus-member.c 2>&1 | FileCheck %s --check-prefix=WHOLEPLUS
// RUN: not emitrust-import-c %t/global-struct.c 2>&1 | FileCheck %s --check-prefix=GLOBAL
// RUN: not emitrust-import-c %t/impure-call-index.c 2>&1 | FileCheck %s --check-prefix=CALLIDX
// RUN: not emitrust-import-c %t/impure-incdec-index.c 2>&1 | FileCheck %s --check-prefix=INCIDX

// FR-74/FR-86 frontier: member-array arguments decay to slice
// parameters ONLY for the provable shapes — a dot/arrow projection
// chain over an addressable LOCAL struct place (whole-member, or
// FR-86's offset forms with a PURE index), borrowing exactly one field
// per borrow. Everything else keeps a located rejection, because each
// of these shapes would otherwise emit code that is wrong or fails only
// downstream: the SAME field twice in one call is two overlapping
// borrows (rustc E0499 after emission); a whole-struct borrow plus a
// member of the same struct overlaps by prefix; a GLOBAL struct's member
// place is a staged local copy, so a mutable slice of it would silently
// lose the callee's writes; and an IMPURE offset index (a call, an
// inc/dec) cannot be evaluated exactly once at the borrow point, so
// the FR-86 interception DECLINES and the historical decay rejection
// fires unchanged. (A pointer LOCAL bound to a member array — the
// non-argument decay position rejected here through FR-92 — is
// admitted by FR-93's member-place backing; its pins live in
// pointers-member-array-local.c and its frontier in
// pointers-member-array-local-invalid.c.) Rejection is a feature:
// every wording below is pinned verbatim as measured.

// The same FIELD twice into two mutable slice parameters: overlapping
// mutable borrows. The (base, field-path) guard collides on the exact
// path; parity with the mutness-blind top-level same-array guard.
// SAMEFIELD: same-field.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 's')

//--- same-field.c
struct S { unsigned char a[8]; unsigned int n; };
static void two(unsigned char *x, unsigned char *y, unsigned len) {
  unsigned i;
  for (i = 0; i < len; i++) {
    x[i] = (unsigned char)(x[i] + y[i]);
    y[i] = 1;
  }
}
int main(void) {
  struct S s;
  s.a[0] = 1;
  two(s.a, s.a, 8u);
  return s.a[0];
}

// A whole-struct borrow and a member array of the SAME struct in one
// call: the empty path is a prefix of every member path, so the guard
// collides — `&mut s` and `&mut s.a` cannot be live at once.
// WHOLEPLUS: whole-plus-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 's')

//--- whole-plus-member.c
struct S { unsigned char a[8]; unsigned int n; };
static void both(struct S *w, unsigned char *x, unsigned len) {
  unsigned i;
  for (i = 0; i < len; i++)
    x[i] = (unsigned char)(x[i] + 1u);
  w->n = len;
}
int main(void) {
  struct S s;
  s.a[0] = 1;
  both(&s, s.a, 8u);
  return s.a[0];
}

// A GLOBAL struct's member array: the member place is a staged local
// copy of the global, so a mutable slice of it would silently lose the
// callee's writes — the chain root must have local storage.
// GLOBAL: global-struct.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- global-struct.c
struct S { unsigned char iv[8]; unsigned int n; };
static struct S g;
static void bump(unsigned char *buf, unsigned len) {
  unsigned i;
  for (i = 0; i < len; i++)
    buf[i] = (unsigned char)(buf[i] + 1u);
}
int main(void) {
  g.n = 8u;
  bump(g.iv, g.n);
  return (int)g.iv[0];
}

// FR-86 purity gate: a function-CALL index cannot be admitted — the
// interception evaluates the index once at the borrow point, and a call
// there would reorder its side effects against the other arguments —
// so it declines and the decay rejection stays verbatim.
// CALLIDX: impure-call-index.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- impure-call-index.c
struct S { unsigned char key[16]; unsigned int n; };
static unsigned int bump_idx(void) { static unsigned int c; return c++; }
static void sink(const unsigned char *p, unsigned int n) {
  unsigned int i;
  for (i = 0; i < n; i++) { if (p[i]) return; }
}
int main(void) {
  struct S s;
  s.key[0] = 1;
  sink(&s.key[bump_idx()], 4u);
  return 0;
}

// FR-86 purity gate: an inc/dec in the index is a side effect the
// single borrow-point evaluation would misplace; declined the same way.
// INCIDX: impure-incdec-index.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- impure-incdec-index.c
struct S { unsigned char key[16]; unsigned int n; };
static void sink(const unsigned char *p, unsigned int n) {
  unsigned int i;
  for (i = 0; i < n; i++) { if (p[i]) return; }
}
void inc_idx(struct S *s, unsigned int i) {
  sink(&s->key[i++], 4u);
  s->n = i;
}

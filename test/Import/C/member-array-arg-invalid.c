// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/same-field.c 2>&1 | FileCheck %s --check-prefix=SAMEFIELD
// RUN: not emitrust-import-c %t/whole-plus-member.c 2>&1 | FileCheck %s --check-prefix=WHOLEPLUS
// RUN: not emitrust-import-c %t/ptr-local.c 2>&1 | FileCheck %s --check-prefix=PTRLOCAL
// RUN: not emitrust-import-c %t/global-struct.c 2>&1 | FileCheck %s --check-prefix=GLOBAL
// RUN: not emitrust-import-c %t/offset-plus.c 2>&1 | FileCheck %s --check-prefix=OFFSETPLUS
// RUN: not emitrust-import-c %t/offset-subscript.c 2>&1 | FileCheck %s --check-prefix=OFFSETSUB

// FR-74 frontier: member-array arguments decay to slice parameters ONLY
// for the provable shapes — a dot/arrow projection chain over an
// addressable LOCAL struct place, borrowing exactly one field per
// borrow. Everything else keeps a located rejection, because each of
// these shapes would otherwise emit code that is wrong or fails only
// downstream: the SAME field twice in one call is two overlapping
// borrows (rustc E0499 after emission); a whole-struct borrow plus a
// member of the same struct overlaps by prefix; a pointer LOCAL bound
// to a member array is a non-argument decay position the pointer
// decomposition has no representation for; a GLOBAL struct's member
// place is a staged local copy, so a mutable slice of it would silently
// lose the callee's writes; and offset forms (`s.iv + 2`, `&s.iv[2]`)
// are not the whole-member cursor-0 shape this wave admits. Rejection
// is a feature: every wording below is pinned verbatim as measured.

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

// Non-argument decay position: a pointer LOCAL bound to a member array
// stays rejected this wave, with the pointer-decomposition wording (NOT
// the ArrayToPointerDecay one — the binding site rejects first).
// PTRLOCAL: ptr-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- ptr-local.c
struct S { unsigned char iv[8]; unsigned int n; };
int main(void) {
  struct S s;
  unsigned char *p = s.iv;
  s.iv[0] = 1;
  return p[0];
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

// Offset arithmetic over the decayed member (`s.iv + 2`): not the
// whole-member cursor-0 shape; keeps the decay rejection verbatim.
// OFFSETPLUS: offset-plus.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- offset-plus.c
struct S { unsigned char iv[8]; unsigned int n; };
static void bump(unsigned char *buf, unsigned len) {
  unsigned i;
  for (i = 0; i < len; i++)
    buf[i] = (unsigned char)(buf[i] + 1u);
}
int main(void) {
  struct S s;
  s.n = 6u;
  s.iv[2] = 1;
  bump(s.iv + 2, s.n);
  return (int)s.iv[2];
}

// The subscript-address spelling of the same offset shape (`&s.iv[2]`).
// OFFSETSUB: offset-subscript.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- offset-subscript.c
struct S { unsigned char iv[8]; unsigned int n; };
static void bump(unsigned char *buf, unsigned len) {
  unsigned i;
  for (i = 0; i < len; i++)
    buf[i] = (unsigned char)(buf[i] + 1u);
}
int main(void) {
  struct S s;
  s.n = 6u;
  s.iv[2] = 1;
  bump(&s.iv[2], s.n);
  return (int)s.iv[2];
}

// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/scalar-member.c 2>&1 | FileCheck %s --check-prefix=SCALAR
// RUN: not emitrust-import-c %t/union-member.c 2>&1 | FileCheck %s --check-prefix=UNION
// RUN: not emitrust-import-c %t/escaping-store.c 2>&1 | FileCheck %s --check-prefix=ESCAPE
// RUN: not emitrust-import-c %t/same-member.c 2>&1 | FileCheck %s --check-prefix=SAMEMEMBER
// RUN: not emitrust-import-c %t/whole-plus-member.c 2>&1 | FileCheck %s --check-prefix=WHOLEPLUS

// FR-90 frontier: member-ADDRESS arguments through a DECOMPOSED
// (null-compared) struct-pointer root are admitted ONLY for the provable
// shape — a dot/arrow projection chain ending at a plain STRUCT-typed
// field, borrowing exactly one field per borrow. Everything else keeps a
// located rejection, because each of these shapes would otherwise emit
// code that is wrong or fails only downstream: a SCALAR member's address
// has no admitted scalar-reference story through the decomposition (the
// interception declines on the leaf, so the historical wording fires
// unchanged); a UNION-typed member's arms overlap by construction, so no
// per-field disjointness argument exists; a pointer LOCAL bound to the
// member address is an escaping store the pointer decomposition has no
// representation for (the binding site rejects first, with the
// assignment-context wording); the SAME member twice in one call is two
// overlapping mutable borrows (rustc E0499 if emitted — the (root,
// field-path) guard collides on the exact path); and a whole-root
// forward plus a member of the same root overlaps by prefix (the empty
// path is a prefix of every member path). Rejection is a feature: every
// wording below is pinned verbatim as measured.

// The address of a SCALAR member to a scalar-pointer parameter: the
// FR-90 interception declines (leaf not struct-typed) and the
// decomposed routing keeps its historical verbatim rejection.
// SCALAR: scalar-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer target expression

//--- scalar-member.c
struct Outer { unsigned int k; unsigned int m; };
static void bumpu(unsigned int *x) { *x += 1u; }
unsigned int drive(struct Outer *o) {
  if (o == (struct Outer *)0) { return 1u; }
  bumpu(&o->k);
  return o->k;
}

// The address of a UNION-typed member: arms overlap, so the leaf gate
// declines and the historical rejection stays.
// UNION: union-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer target expression

//--- union-member.c
union Blob { unsigned int u; unsigned char b[4]; };
struct Outer { union Blob h; unsigned int k; };
static void bump(union Blob *x) { x->u += 1u; }
unsigned int drive(struct Outer *o) {
  if (o == (struct Outer *)0) { return 1u; }
  bump(&o->h);
  return o->h.u;
}

// An ESCAPING STORE of the member address (a pointer local bound to
// `&o->h`): a non-argument position the decomposition has no
// representation for — the binding site rejects first, with the
// assignment-context wording.
// ESCAPE: escaping-store.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- escaping-store.c
struct Inner { unsigned int a; unsigned int b; };
struct Outer { struct Inner h; unsigned int k; };
unsigned int drive(struct Outer *o) {
  if (o == (struct Outer *)0) { return 1u; }
  struct Inner *p = &o->h;
  return p->a;
}

// The SAME member twice into two mutable struct-pointer parameters:
// overlapping mutable borrows — the (root, field-path) guard collides
// on the exact path.
// SAMEMEMBER: same-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 'o')

//--- same-member.c
struct Inner { unsigned int a; unsigned int b; };
struct Outer { struct Inner h; struct Inner g; };
static void two(struct Inner *x, struct Inner *y) { x->a += y->a; }
unsigned int drive(struct Outer *o) {
  if (o == (struct Outer *)0) { return 1u; }
  two(&o->h, &o->h);
  return o->h.a;
}

// A whole-root forward plus a member of the SAME decomposed root in one
// call: the empty path is a prefix of every member path, so the guard
// collides — `&mut *o` and `&mut o.h` cannot be live at once. (Before
// FR-90 this shape rejected as "unsupported pointer target expression"
// on the member argument; the guard's located rejection replaces it —
// the pin moved forward, it never loosened.)
// WHOLEPLUS: whole-plus-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 'o')

//--- whole-plus-member.c
struct Inner { unsigned int a; unsigned int b; };
struct Outer { struct Inner h; struct Inner g; };
static void mix(struct Outer *w, struct Inner *x) { x->a += w->g.a; }
unsigned int drive(struct Outer *o) {
  if (o == (struct Outer *)0) { return 1u; }
  mix(o, &o->h);
  return o->h.a;
}

// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/elem.c 2>&1 | FileCheck %s --check-prefix=ELEM
// RUN: not emitrust-import-c %t/impure.c 2>&1 | FileCheck %s --check-prefix=IMPURE
// RUN: not emitrust-import-c %t/scalar.c 2>&1 | FileCheck %s --check-prefix=SCALAR
// RUN: not emitrust-import-c %t/alias.c 2>&1 | FileCheck %s --check-prefix=ALIAS
// RUN: not emitrust-import-c %t/literal.c 2>&1 | FileCheck %s --check-prefix=LITERAL
// RUN: not emitrust-import-c %t/byteregion.c 2>&1 | FileCheck %s --check-prefix=BYTEREGION

// FR-89 frontier: the void*-mediated cast peel on the borrow-argument
// path admits EXACTLY the member-array shapes the FR-74/86 matcher
// already proves, with element agreement to the admitted byte element
// and the (root, field-path) aliasing guard composing unchanged.
// Everything else keeps a located rejection, because each of these
// shapes would otherwise emit code that is wrong or fails only
// downstream: a NON-BYTE member element does not match the admitted
// ui8 slice element (the peel exposes the member place, so the
// rejection MOVES from the historical decay wording to the in-source
// element check — a strictly more precise diagnosis); an IMPURE offset
// index cannot be evaluated exactly once at the borrow point, so the
// matcher declines and the historical decay rejection fires unchanged;
// the address of a SCALAR object is not a byte view (the hmac_prng.c
// tu0_update `&separator0` blocker — this FR does NOT unblock it); the
// SAME field twice into two mutable void* params is two overlapping
// borrows, and the wording FLIPS from the decay rejection to the
// aliasing guard's (the pin moves forward: the guard now sees the
// shape); a STRING LITERAL to a ui8-element void* param keeps its
// exact pre-FR-89 element-mismatch rejection (char is i8 — the peel
// sits after the literal head and provably admits nothing new there);
// and an ALL-u8 struct's member array is the byte-region-aggregate
// boundary (CTS-BR), which rejects at decay even on the TYPED path —
// FR-89 must not move that boundary. Rejection is a feature: every
// wording below is pinned verbatim as measured against the built tool.

// A non-byte (unsigned, ui32) member element into the admitted ui8
// byte-cursor param: the element check rejects, per call, never a
// silent cast.
// ELEM: elem.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: argument element type does not match the slice parameter

//--- elem.c
typedef unsigned char u8;
struct T { unsigned w[4]; unsigned n; };
static void bset(void *to, u8 val, unsigned len) {
  u8 *d = (u8 *)to;
  unsigned i;
  for (i = 0; i < len; i++)
    d[i] = val;
}
int main(void) {
  struct T t;
  t.n = 0u;
  bset(t.w, 1, 16u);
  return (int)t.n;
}

// FR-86 purity gate composes: a function-CALL offset index declines the
// matcher, so the historical decay rejection fires unchanged.
// IMPURE: impure.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- impure.c
typedef unsigned char u8;
struct S { u8 iv[16]; unsigned n; };
static unsigned bump(void) { static unsigned k; return k++; }
static void bset(void *to, u8 val, unsigned len) {
  u8 *d = (u8 *)to;
  unsigned i;
  for (i = 0; i < len; i++)
    d[i] = val;
}
int main(void) {
  struct S s;
  s.n = 0u;
  bset(s.iv + bump(), 1, 4u);
  return (int)s.n;
}

// The address of a SCALAR object is not a byte view of the admitted
// param — hmac_prng.c:89's `&separator0` shape stays a located
// rejection (tu0_update does not unblock this wave).
// SCALAR: scalar.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the address of a scalar object cannot be passed as a slice parameter

//--- scalar.c
typedef unsigned char u8;
static u8 rd(const void *from, unsigned len) {
  const u8 *s = (const u8 *)from;
  u8 acc = 0;
  unsigned i;
  for (i = 0; i < len; i++)
    acc = (u8)(acc + s[i]);
  return acc;
}
int main(void) {
  const u8 sep = 0;
  return (int)rd(&sep, 1u);
}

// The SAME field twice into two mutable void* params: the (root,
// field-path) guard collides on the exact path. FLIPPED pin: this
// shape rejected at the decay before FR-89; the peel now walks it into
// the guard, whose wording is strictly more precise.
// ALIAS: alias.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 's')

//--- alias.c
typedef unsigned char u8;
struct S { u8 iv[16]; unsigned n; };
static void two(void *a, void *b, unsigned len) {
  u8 *x = (u8 *)a;
  u8 *y = (u8 *)b;
  unsigned i;
  for (i = 0; i < len; i++) {
    x[i] = 1;
    y[i] = 2;
  }
}
int main(void) {
  struct S s;
  s.n = 0u;
  two(s.iv, s.iv + 8, 8u);
  return (int)s.n;
}

// A string literal to a ui8-element void* param: char's i8 element
// disagrees with the admitted ui8 — the exact pre-FR-89 rejection, in
// wording and in path (the peel sits AFTER the literal head).
// LITERAL: literal.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: argument element type does not match the slice parameter

//--- literal.c
typedef unsigned char u8;
static u8 rd(const void *from, unsigned len) {
  const u8 *s = (const u8 *)from;
  u8 acc = 0;
  unsigned i;
  for (i = 0; i < len; i++)
    acc = (u8)(acc + s[i]);
  return acc;
}
int main(void) {
  return (int)rd("ab", 2u);
}

// An ALL-u8 struct is a byte-region aggregate (CTS-BR): its member
// place is not projectable, the matcher declines, and the decay
// rejection stays — identical to the TYPED-path boundary, which FR-89
// must not move.
// BYTEREGION: byteregion.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- byteregion.c
typedef unsigned char u8;
struct B { u8 a[8]; u8 b[8]; };
static void two(void *p, void *q, unsigned len) {
  u8 *x = (u8 *)p;
  u8 *y = (u8 *)q;
  unsigned i;
  for (i = 0; i < len; i++) {
    x[i] = 1;
    y[i] = 2;
  }
}
int main(void) {
  struct B s;
  two(s.a, s.b, 8u);
  return 0;
}

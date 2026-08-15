// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/two-windows.c 2>&1 | FileCheck %s --check-prefix=TWOWIN
// RUN: not emitrust-import-c %t/fam.c 2>&1 | FileCheck %s --check-prefix=FAM
// RUN: not emitrust-import-c %t/const-mut.c 2>&1 | FileCheck %s --check-prefix=CONSTMUT
// RUN: not emitrust-import-c %t/global-root.c 2>&1 | FileCheck %s --check-prefix=GLOBAL
// RUN: not emitrust-import-c %t/impure-index.c 2>&1 | FileCheck %s --check-prefix=IMPURE

// FR-91 frontier: byte-region member WINDOWS admit exactly the shapes
// the region model renders as safe Rust; everything else keeps a
// LOCATED rejection — rejection is a feature, and none of these
// shapes may silently emit code that is wrong or fails only at rustc.
// Windows of one region share ONE slice place, so TWO windows of the
// same root in one call with a mut among them is rustc E0502 (the
// FR-74 disjoint-sibling two-borrow admission does NOT transfer to
// byte-region roots) — the call rejects at the root-keyed aliasing
// guard, whose wording the guard already owns. A FLEXIBLE ARRAY
// MEMBER leaf has no extent (the heatshrink hsd->buffers sites), so
// the window matcher declines exactly like the typed path's
// ConstantArrayType gate and the historical decay rejection stays
// verbatim. A MUT window cannot borrow through a SHARED
// (const-pointee) region; a GLOBAL root's window would be a staged
// copy whose writes are silently lost; and the FR-86 purity gate
// composes unchanged (an impure offset index declines) — all three
// decline into the historical decay rejection. Every wording below is
// pinned verbatim as measured against the built tool.

// Two windows of one region with a mut: both arguments resolve to
// `slice_of` views of the SAME slice place, so the aliasing guard
// keys on the root alone (empty field path) and collides.
// TWOWIN: two-windows.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 's')

//--- two-windows.c
typedef unsigned char u8;
struct B { u8 a[8]; u8 b[8]; };
static void two(u8 *x, u8 *y) {
  x[0] = 1;
  y[0] = 2;
}
int main(void) {
  struct B s;
  two(s.a, s.b);
  return 0;
}

// A flexible array member on an (all-u8, still byte-region-classified)
// record: no extent, no window — the matcher declines and the decay
// rejection stays, matching the heatshrink FAM census sites.
// FAM: fam.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- fam.c
#include <string.h>
typedef unsigned char u8;
struct H { u8 head[4]; u8 tail[]; };
void f(struct H *h, unsigned n) {
  memset(h->tail, 0, n);
}

// A MUT window (memset destination) through a CONST byte-region
// pointer: a shared region cannot yield `&mut`, so the matcher
// declines into the historical located path.
// CONSTMUT: const-mut.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- const-mut.c
typedef unsigned char u8;
struct B { u8 a[8]; u8 b[8]; };
static void bump(u8 *p) { p[0] = 1; }
void f(const struct B *c) {
  bump((void *)c->a);
}

// A GLOBAL byte-region root: the region place would be a staged local
// copy of the global, and a mutable window of the copy would silently
// lose the callee's writes — the chain root must have local storage
// (same rule as the typed member path).
// GLOBAL: global-root.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- global-root.c
#include <string.h>
typedef unsigned char u8;
struct B { u8 a[8]; u8 b[8]; };
static struct B g;
int main(void) {
  memset(g.a, 0, 8);
  return (int)(g.a[0] & 1u);
}

// FR-86 purity gate on the window's offset index: a CALL index cannot
// be evaluated exactly once at the borrow point without reordering its
// side effects, so the window declines and the decay rejection fires
// unchanged.
// IMPURE: impure-index.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- impure-index.c
#include <string.h>
typedef unsigned char u8;
struct B { u8 a[8]; u8 b[8]; };
static unsigned bump(void) { static unsigned k; return k++; }
void f(struct B *c) {
  memset(&c->a[bump()], 0, 2);
}

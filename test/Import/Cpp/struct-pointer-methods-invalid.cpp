// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/multi-object.cpp 2>&1 | FileCheck %s --check-prefix=MULTIOBJ
// RUN: not emitrust-import-c %t/region-join.cpp 2>&1 | FileCheck %s --check-prefix=JOIN
// RUN: not emitrust-import-c %t/rebind-upcast.cpp 2>&1 | FileCheck %s --check-prefix=REBIND
// RUN: not emitrust-import-c %t/global-mutating.cpp 2>&1 | FileCheck %s --check-prefix=GLOBALMUT
// RUN: not emitrust-import-c %t/global-upcast-init.cpp 2>&1 | FileCheck %s --check-prefix=GLOBALINIT
// RUN: not emitrust-import-c %t/empty-base-upcast.cpp 2>&1 | FileCheck %s --check-prefix=EMPTYUP
// RUN: not emitrust-import-c %t/base-param.cpp 2>&1 | FileCheck %s --check-prefix=BASEPARAM
// RUN: not emitrust-import-c %t/base-member.cpp 2>&1 | FileCheck %s --check-prefix=BASEMEMBER
// RUN: not emitrust-import-c %t/base-array.cpp 2>&1 | FileCheck %s --check-prefix=BASEARRAY

// FR-120 located-rejection ledger for struct-pointer method calls and
// upcast bindings. FR-120 admitted exactly TWO pointer shapes -- a method
// call through a single-object LOCAL struct pointer (or pointer
// parameter), and an upcast binding `Base *p = &d;` over a single,
// public, non-virtual, NON-POLYMORPHIC base chain -- because those are
// the shapes measured byte-identical against `clang++ -std=c++17`
// (cpp-struct-pointer-methods.cpp, cpp-upcast-pointer.cpp). Everything
// below stays a located rejection; nothing may fall through to the old
// blanket "unsupported use of pointer variable" and nothing may silently
// emit wrong code.
//
// The measured reason, one per case:
//
// * a pointer bound to MULTIPLE objects: the enum-of-bases deref model
//   only stages SCALAR elements, and a method receiver needs a real
//   struct place. Fires THROUGH the new method-call receiver path, at
//   the deref -- the soundness fence the receiver interception must not
//   bypass.
// * a REGION JOIN across two DIFFERENT derived types: two upcast
//   bindings would put objects of two types into one region; the peel
//   admits each binding and the planner's join rejection names both
//   objects and both sites (post-peel it lands HERE, no longer on the
//   non-address wording).
// * pointer REBINDING across an upcast (same derived type twice): only
//   initializer forms are admitted; the bases are not "degenerate
//   scalars of the pointee type" (they are DERIVED objects viewed as
//   the base), so the same join rejection fires.
// * a MUTATING method through a pointer to a GLOBAL object: a global
//   base resolves through a staged copy and the method-call path has no
//   writeback flush -- staging would silently drop the mutation, so it
//   gets its own wording (the CONST sibling is admitted and byte-diffed
//   in cpp-struct-pointer-methods.cpp). Mirrored in RejectionLedger.cpp
//   as `cxx-method-global-receiver`.
// * a GLOBAL upcast pointer: the globals planner keeps its own,
//   unrelaxed exact-type check -- global base pointers stay out both
//   ways (a global pointer to a LOCAL object keeps the escaping-borrow
//   rejection, pinned on the C side).
// * an upcast over an EMPTY base: `Tag *p = &d;` binds, but the access
//   reconciles into the hop screens, which key on the AST's `isEmpty()`
//   (kept deliberately conservative by W2.26 -- materialization of
//   droppy empties does not move them); same wording as the direct
//   empty-base access.
// * base-pointer PARAMETERS / MEMBERS / ARRAYS: unchanged frontiers --
//   each keeps its pre-FR-120 rejection (argument-type mismatch, the
//   member planner's non-address wording, the no-parameter-position
//   array wording). The polymorphic-chain fence (a virtual-dtor base
//   pointer must NOT bind) is pinned in inheritance-drop-invalid.cpp's
//   VUPCAST arm, and slicing still dies at the copy/move gate
//   (inheritance-invalid.cpp SLICECOPY).

//--- multi-object.cpp
struct Counter {
  int n;
  int get() const { return n; }
  void bump(int d) { n += d; }
};
int use(int k) {
  Counter a, b;
  a.n = k;
  b.n = k + 1;
  Counter *p = &a;
  if (k) p = &b;
  // MULTIOBJ: multi-object.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: dereference of a pointer bound to multiple objects with a non-scalar element type
  p->bump(1);
  return p->get();
}

//--- region-join.cpp
struct A { int a; int geta() const { return a; } };
struct D1 : A { int x; };
struct D2 : A { int y; };
int use(int k) {
  D1 d1; d1.a = k;
  D2 d2; d2.a = k + 1;
  // JOIN: region-join.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer 'p' would join objects 'd1' and 'd2' into one region
  A *p = &d1;
  if (k) p = &d2;
  return p->geta();
}

//--- rebind-upcast.cpp
struct A { int a; int geta() const { return a; } };
struct D : A { int y; };
int use(int k) {
  D d1; d1.a = k;
  D d2; d2.a = k + 1;
  // REBIND: rebind-upcast.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer 'p' would join objects 'd1' and 'd2' into one region
  A *p = &d1;
  p = &d2;
  return p->geta();
}

//--- global-mutating.cpp
struct Counter {
  int n;
  int get() const { return n; }
  void bump(int d) { n += d; }
};
Counter g;
int use(int k) {
  Counter *p = &g;
  // GLOBALMUT: global-mutating.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: mutating method call through a pointer to a global object
  p->bump(k);
  return p->get();
}

//--- global-upcast-init.cpp
struct A { int a; int geta() const { return a; } };
struct D : A { int y; };
D d;
// GLOBALINIT: global-upcast-init.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type does not match its target object
A *gp = &d;
int use(int k) { return gp->geta() + k; }

//--- empty-base-upcast.cpp
struct Tag { int id() const { return 1; } };
struct D : Tag { int c; };
int use(int k) {
  D d;
  d.c = k;
  Tag *p = &d;
  // EMPTYUP: empty-base-upcast.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: inherited member of an empty base class
  return p->id() + d.c;
}

//--- base-param.cpp
struct A { int a; int geta() const { return a; } };
struct D : A { int y; };
static int reads(A *p) { return p->geta(); }
int use(int k) {
  D d; d.a = k;
  // BASEPARAM: base-param.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: argument type does not match the pointer parameter
  return reads(&d);
}

//--- base-member.cpp
struct A { int a; };
struct D : A { int y; };
struct Holder { A *p; };
int use(int k) {
  D d; d.a = k;
  Holder h;
  // BASEMEMBER: base-member.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member assigned a non-address value
  h.p = &d;
  return h.p->a;
}

//--- base-array.cpp
struct A { int a; };
struct D : A { int y; };
int use(int k) {
  D d; d.a = k;
  // BASEARRAY: base-array.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position
  A *arr[1] = { &d };
  return arr[0]->a;
}

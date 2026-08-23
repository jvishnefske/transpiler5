// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/ptr-param.cpp 2>&1 | FileCheck %s --check-prefix=PTRPARAM
// RUN: not emitrust-import-c %t/implicit-this.cpp 2>&1 | FileCheck %s --check-prefix=THISCALL
// RUN: not emitrust-import-c %t/ctor-body.cpp 2>&1 | FileCheck %s --check-prefix=CTORCALL
// RUN: not emitrust-import-c %t/two-object-dynamic.cpp 2>&1 | FileCheck %s --check-prefix=TWOOBJ
// RUN: not emitrust-import-c %t/exact-two-virtual.cpp 2>&1 | FileCheck %s --check-prefix=EXACTVIRT
// RUN: not emitrust-import-c %t/exact-two-nonvirtual.cpp 2>&1 | FileCheck %s --check-prefix=EXACTNONV
// RUN: not emitrust-import-c %t/global-object-virtual.cpp 2>&1 | FileCheck %s --check-prefix=GLOBALVIRT
// RUN: not emitrust-import-c %t/nullable-pointer.cpp 2>&1 | FileCheck %s --check-prefix=NULLABLEPTR
// RUN: not emitrust-import-c %t/qualified-virtual-ptr.cpp 2>&1 | FileCheck %s --check-prefix=QUALPTR
// RUN: not emitrust-import-c %t/sizeof-multi-virtual.cpp 2>&1 | FileCheck %s --check-prefix=SIZEOFMV
// RUN: not emitrust-import-c %t/base-reference.cpp 2>&1 | FileCheck %s --check-prefix=BASEREF
// RUN: not emitrust-import-c %t/new-delete.cpp 2>&1 | FileCheck %s --check-prefix=NEWDEL
// RUN: not emitrust-import-c %t/box-receiver.cpp 2>&1 | FileCheck %s --check-prefix=BOXPTR
// RUN: not emitrust-cc --emit=import %t/pure-qualified-call.cpp -o - 2>&1 | FileCheck %s --check-prefix=PUREUSE
// RUN: not emitrust-import-c %t/abstract-value.cpp 2>&1 | FileCheck %s --check-prefix=ABSTRACT

// W2.19a/W2.19b located-rejection ledger for virtual methods. W2.19a
// admitted virtual methods on VALUES (static bind is exact: a value's
// static type IS its dynamic type); W2.19b admitted virtual calls
// through a pointer whose region binds EXACTLY ONE local object (the
// FR-120 single-object fact: the object's dynamic type is statically
// known, so the call DEVIRTUALIZES to its final overrider -- pinned
// positive in virtual-methods-devirt.cpp). Everything below is a channel
// where the receiver's dynamic type is NOT statically known -- or where
// the emission model cannot carry the resolved call -- and each stays a
// located rejection; nothing may silently emit a statically-bound call
// that C++ would dispatch dynamically.
//
// The measured reason, one per case:
//
// * a virtual call through a pointer-shaped receiver whose one object is
//   NOT statically known is the W2.19a soundness fence, unchanged in
//   `emitCXXMemberCall`'s receiver lambda: a pointer PARAMETER (the
//   caller set is open -- any caller may pass a derived object), and
//   implicit/explicit `this` (the measured W2.19a miscompile: without
//   the fence `d.callf()` -- where `B::callf` returns `this->f()` and
//   `D` overrides `f` -- compiles clean and prints 1 where the native
//   prints 3). The ctor-body call stays deliberately over-rejected (C++
//   ctor semantics equal static bind -- a recorded future carve-out
//   candidate, not this wave). The W2.21 unique_ptr receiver (`bp->f()`
//   over a Box) is intercepted ABOVE the lambda and KEEPS its own copy
//   of the fence: the payload type is exact today (only the same-T
//   `make_unique<T>` initializer is recognized), which would make devirt
//   free -- but that is an invariant no byte-diff oracle guards and no
//   REGION fact backs (the Box has no single-object region), so the
//   call stays over-rejected until one does.
// * a pointer bound to TWO objects of DIFFERENT dynamic types is THE
//   soundness floor of devirtualization -- the case where devirt would
//   pick wrong. Post-peel the two upcast bindings land in the planner's
//   multi-base model, whose element-type uniformity check (derived
//   objects viewed as the base are not "degenerate objects of the
//   pointee type") fires the JOIN rejection naming both objects and
//   both sites. The exact-type twin (two objects of ONE polymorphic
//   type) splits by call kind: a VIRTUAL call keeps the fence wording
//   (devirt requires the single-object fact), a NON-virtual call rides
//   to the deref and keeps the historical MULTIOBJ wording.
// * a pointer to a GLOBAL polymorphic object: closed upstream -- a
//   polymorphic class's default constructor is non-trivial (vptr), so
//   the GLOBAL itself is a located rejection and the pointer never
//   binds; the devirt gate additionally requires a LOCAL base, so the
//   fence backstops any future global admission (the globals planner
//   itself stays unrelaxed both ways, pinned in
//   struct-pointer-methods-invalid.cpp).
// * a NULLABLE region (a null constant ever assigned): the devirt gate
//   keys on the plain degenerate single-object binding only; the
//   Option-of-cursor shape keeps the fence.
// * a QUALIFIED call through the pointer (`p->B::f()`): C++ binds it
//   statically, so devirtualizing to the override would be a MISCOMPILE;
//   the gate skips qualified callees and the fence holds (deliberate
//   over-reject -- admitting the static bind is a future carve-out).
// * sizeof/alignof of a polymorphic class: the W2.26 screen, which keyed
//   on `isPolymorphic()` from day one and so covers multi-virtual
//   classes with no change -- pinned here because the class-level gate
//   used to win first and the screen is now the only thing standing.
// * a base REFERENCE (`B &r = d;`): reference locals are not modeled at
//   all; the pre-existing rejection is unchanged.
// * new/delete of a polymorphic class: a new-expression is not an
//   address the pointer planner accepts; unchanged.
// * a QUALIFIED call to a body-less PURE virtual (`i.Pure::f()`): the
//   pure method rides the FR-47 stub channel (omitted when uncalled,
//   pinned positive in virtual-methods-values.cpp); the only way to call
//   it fails LOUDLY at project finalization under the FR-52 marker
//   contract. Project-level, so driven through `emitrust-cc`.
// * direct instantiation of an ABSTRACT class: clang rejects it upstream
//   of the importer, located; the importer never sees a pure-virtual
//   VALUE, which is why admitting the abstract base as a base-as-field
//   subobject is sound.

//--- ptr-param.cpp
struct B {
  int k;
  virtual int f() { return 1 + k; }
};
// PTRPARAM: ptr-param.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: virtual method call through a pointer
int callit(B *p) { return p->f(); }

//--- implicit-this.cpp
// The MEASURED miscompile shape (native 3, unfenced image 1): `callf`'s
// implicit-`this` receiver is a CXXThisExpr -- pointer-typed, so it takes
// the same fence. Inside an ordinary METHOD body the failure rides the
// FR-112 containment channel: the method is OMITTED with the warning
// below, the class and its siblings import, and the USE is the located
// error.
// THISCALL: implicit-this.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: virtual method call through a pointer (omitted: method 'callf' of class 'B')
// THISCALL: implicit-this.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to unimported method 'B_callf'
struct B {
  int k;
  virtual int f() { return 1 + k; }
  int callf() { return f(); }
};
struct D : B {
  int f() override { return 3 + k; }
};
int use() {
  D d;
  d.k = 0;
  return d.callf();
}

//--- ctor-body.cpp
// A virtual call inside a CONSTRUCTOR body: C++'s own semantics bind it
// statically (the dynamic type IS the class under construction), so this
// shape is semantically a future carve-out -- but it is REJECTED this
// wave, located at the call, because a constructor cannot ride the
// FR-112 omission channel and the fence deliberately keys on the
// pointer-shaped receiver alone. This is the explicit ctor rejection the
// W2.19 spike record asked for.
// CTORCALL: ctor-body.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: virtual method call through a pointer
struct B {
  int r;
  virtual int f() { return 1; }
  B() { r = f(); }
};
int use() {
  B b;
  return b.r;
}

//--- two-object-dynamic.cpp
// THE soundness floor of devirtualization: one base pointer, two objects
// of DIFFERENT dynamic types -- the case where resolving the call
// against either object's type would miscompile the other path. The
// relaxed peel admits BOTH bindings, so the wall is the planner's
// multi-base uniformity check: derived objects viewed as the base are
// not degenerate objects of the pointee type, so the JOIN rejection
// names both objects and both sites (the W2.19 spike verified this
// fence is what keeps the single-object devirt sound).
struct A {
  int a;
  virtual int get() { return a; }
};
struct D1 : A {
  int get() override { return a + 1; }
};
struct D2 : A {
  int get() override { return a + 2; }
};
int use(int k) {
  D1 d1;
  d1.a = k;
  D2 d2;
  d2.a = k + 1;
  // TWOOBJ: two-object-dynamic.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer 'p' would join objects 'd1' and 'd2' into one region
  A *p = &d1;
  if (k) p = &d2;
  return p->get();
}

//--- exact-two-virtual.cpp
// The exact-type twin of the soundness floor: two objects of ONE
// polymorphic type. The dynamic types agree, so devirt would happen to
// be right -- but the gate keys on the single-object REGION FACT, not on
// type agreement, and a multi-bound pointer keeps the fence.
struct S {
  int k;
  virtual int f() { return 1 + k; }
};
int use(int n) {
  S a, b;
  a.k = n;
  b.k = n + 1;
  S *p = &a;
  if (n) p = &b;
  // EXACTVIRT: exact-two-virtual.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: virtual method call through a pointer
  return p->f();
}

//--- exact-two-nonvirtual.cpp
// The NON-virtual sibling rides past the fence to the deref and keeps
// the historical multi-object wording (struct-pointer-methods-invalid's
// MULTIOBJ, unchanged for a polymorphic class).
struct S {
  int k;
  virtual int f() { return 1 + k; }
  int g() { return 2 + k; }
};
int use(int n) {
  S a, b;
  a.k = n;
  b.k = n + 1;
  S *p = &a;
  if (n) p = &b;
  // EXACTNONV: exact-two-nonvirtual.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: dereference of a pointer bound to multiple objects with a non-scalar element type
  return p->g();
}

//--- global-object-virtual.cpp
// The GLOBAL-object channel is closed UPSTREAM of the fence: a
// polymorphic class has a non-trivial default constructor (the vptr),
// so no global of it imports at all -- the located rejection sits at
// the global's own declaration and a pointer-to-global virtual call is
// unreachable (the single-object devirt gate additionally requires a
// LOCAL base, so if globals of polymorphic classes ever admit, the
// fence below this pin takes over; the mutating sibling is GLOBALMUT
// and global POINTERS stay out at the globals planner, GLOBALINIT --
// both in struct-pointer-methods-invalid.cpp).
struct S {
  int k;
  virtual int f() const { return 1 + k; }
};
// GLOBALVIRT: global-object-virtual.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-constant global initializer
S g;
int use(int n) {
  S *p = &g;
  return p->f() + n;
}

//--- nullable-pointer.cpp
// A NULLABLE region (a null constant ever assigned): still one bound
// object, but the devirt gate keys on the plain degenerate binding only
// -- the Option-of-cursor shape keeps the fence (conservative; a null
// deref is UB, so admitting it would be legal, but nothing measures it).
struct S {
  int k;
  virtual int f() { return 1 + k; }
};
int use(int n) {
  S s;
  s.k = n;
  S *p = &s;
  if (n) p = 0;
  // NULLABLEPTR: nullable-pointer.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: virtual method call through a pointer
  return p->f();
}

//--- qualified-virtual-ptr.cpp
// A QUALIFIED call through a devirtualizable pointer: C++ binds
// `p->A::get()` STATICALLY to A's body, so devirtualizing to D's
// override would be a miscompile -- the gate skips qualified callees and
// the fence holds. Deliberate over-reject: admitting the static bind is
// a future carve-out, and this pin is what keeps it from flipping
// silently.
struct A {
  int a;
  virtual int get() { return a; }
};
struct D : A {
  int c;
  int get() override { return c; }
};
int use(int n) {
  D d;
  d.a = n;
  d.c = n + 1;
  A *p = &d;
  // QUALPTR: qualified-virtual-ptr.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: virtual method call through a pointer
  return p->A::get();
}

//--- sizeof-multi-virtual.cpp
struct V {
  int id;
  virtual int f() { return id; }
  virtual int g() { return id + 1; }
};
int use(int n) {
  V v;
  v.id = n;
  // SIZEOFMV: sizeof-multi-virtual.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: sizeof/alignof of a polymorphic class
  return (int)sizeof(V) + v.id;
}

//--- base-reference.cpp
struct B {
  int k;
  virtual int f() { return 1 + k; }
};
struct D : B {
  int f() override { return 3 + k; }
};
// BASEREF: base-reference.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference types are not yet supported
int use(int n) {
  D d;
  d.k = n;
  B &r = d;
  return r.f();
}

//--- new-delete.cpp
struct S {
  int k;
  virtual int f() { return k; }
};
// NEWDEL: new-delete.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value
int use(int n) {
  S *p = new S;
  p->k = n;
  int r = p->k;
  delete p;
  return r;
}

//--- box-receiver.cpp
// The Box-payload spelling of the fence, plus the upcast initializer
// that would break the same-T payload invariant rejecting on its own.
#include <memory>
struct B {
  int k;
  virtual int f() { return 1 + k; }
};
struct D : B {
  int f() override { return 3 + k; }
};
// BOXPTR: box-receiver.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: virtual method call through a pointer
int use() {
  std::unique_ptr<B> p = std::make_unique<B>();
  p->k = 2;
  return p->f();
}

//--- pure-qualified-call.cpp
// PUREUSE: error: unsupported: function 'pure_f' is referenced but not defined in any translation unit
struct Pure {
  int x;
  virtual int f() = 0;
};
struct Impl : Pure {
  int f() override { return 5 + x; }
};
int use(int n) {
  Impl i;
  i.x = n;
  return i.Pure::f();
}

//--- abstract-value.cpp
// ABSTRACT: abstract-value.cpp:{{[0-9]+}}:{{[0-9]+}}: error: variable type 'Pure' is an abstract class
struct Pure {
  int x;
  virtual int f() = 0;
};
int use() {
  Pure p;
  return 0;
}

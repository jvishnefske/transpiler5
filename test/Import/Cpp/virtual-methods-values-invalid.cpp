// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/ptr-local.cpp 2>&1 | FileCheck %s --check-prefix=PTRLOCAL
// RUN: not emitrust-import-c %t/ptr-deref.cpp 2>&1 | FileCheck %s --check-prefix=PTRDEREF
// RUN: not emitrust-import-c %t/ptr-param.cpp 2>&1 | FileCheck %s --check-prefix=PTRPARAM
// RUN: not emitrust-import-c %t/implicit-this.cpp 2>&1 | FileCheck %s --check-prefix=THISCALL
// RUN: not emitrust-import-c %t/ctor-body.cpp 2>&1 | FileCheck %s --check-prefix=CTORCALL
// RUN: not emitrust-import-c %t/upcast-virtual-method.cpp 2>&1 | FileCheck %s --check-prefix=VUPCASTM
// RUN: not emitrust-import-c %t/sizeof-multi-virtual.cpp 2>&1 | FileCheck %s --check-prefix=SIZEOFMV
// RUN: not emitrust-import-c %t/base-reference.cpp 2>&1 | FileCheck %s --check-prefix=BASEREF
// RUN: not emitrust-import-c %t/new-delete.cpp 2>&1 | FileCheck %s --check-prefix=NEWDEL
// RUN: not emitrust-import-c %t/box-receiver.cpp 2>&1 | FileCheck %s --check-prefix=BOXPTR
// RUN: not emitrust-cc --emit=import %t/pure-qualified-call.cpp -o - 2>&1 | FileCheck %s --check-prefix=PUREUSE
// RUN: not emitrust-import-c %t/abstract-value.cpp 2>&1 | FileCheck %s --check-prefix=ABSTRACT

// W2.19a located-rejection ledger for virtual methods on values. The wave
// admits a class with virtual methods (override/final, multi-level) when
// every use is a VALUE use, with calls statically bound to the receiver's
// own type's override -- correct because a value's static type IS its
// dynamic type. Everything below is a channel where static and dynamic
// type COULD diverge, and each stays a located rejection; nothing may
// silently emit a statically-bound call that C++ would dispatch
// dynamically.
//
// The measured reason, one per case:
//
// * a virtual call through ANY pointer-shaped receiver -- local `p->f()`,
//   `(*p).f()`, a pointer PARAMETER, implicit/explicit `this`, and a
//   constructor body -- is THE soundness fence this wave adds, in
//   `emitCXXMemberCall`'s receiver lambda after `pointerExpr` is
//   computed, so every pointer spelling funnels through one check. The
//   spike MEASURED the miscompile that fence prevents: with the
//   class-level gate lifted and no fence, `d.callf()` (where `B::callf`
//   returns `this->f()` and `D` overrides `f`) compiles clean and prints
//   1 where the native prints 3 -- the hop projection upcasts the
//   receiver in a way `uniquePublicSingleBaseHops` never sees, and
//   `this->f()` binds the BASE's body. Three of the pointer shapes below
//   are deliberately over-rejected even though the spike measured them
//   accidentally correct today: a same-type local pointer (W2.19b's
//   scope), a same-type parameter (unsound across callers in principle),
//   and a ctor-body call (C++ ctor semantics equal static bind -- a
//   future carve-out candidate, not this wave). The W2.21 unique_ptr
//   receiver (`bp->f()` over a Box) is intercepted ABOVE the lambda, so
//   it carries its own copy of the fence: today only the same-T
//   `make_unique<T>` initializer is recognized (the payload type is
//   exact -- the upcast initializer is its own rejection, pinned below),
//   but that is an invariant no byte-diff oracle guards, so the call
//   over-rejects like the raw-pointer shapes.
// * the polymorphic UPCAST: unchanged VUPCAST fence, now covering
//   virtual-METHOD chains too -- `uniquePublicSingleBaseHops` refuses
//   every hop of a polymorphic chain (`isPolymorphic()`), so the binding
//   falls through to the planner's non-address wording. Post-lift this
//   predicate is the SOLE fence keeping pointer-static-type ==
//   pointee-dynamic-type, the invariant that makes value-side static
//   binding sound. Zero code change; the pin is new because the admitted
//   set around it widened.
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

//--- ptr-local.cpp
struct B {
  int k;
  virtual int f() { return 1 + k; }
};
// PTRLOCAL: ptr-local.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: virtual method call through a pointer
int use(int n) {
  B b;
  b.k = n;
  B *p = &b;
  return p->f();
}

//--- ptr-deref.cpp
struct B {
  int k;
  virtual int f() { return 1 + k; }
};
// PTRDEREF: ptr-deref.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: virtual method call through a pointer
int use(int n) {
  B b;
  b.k = n;
  B *p = &b;
  return (*p).f();
}

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

//--- upcast-virtual-method.cpp
// The VUPCAST shape over a virtual-METHOD chain (inheritance-drop-
// invalid.cpp pins the sole-virtual-dtor twin): both classes are now
// ADMITTED values, so the only fence left is the predicate itself.
// VUPCASTM: upcast-virtual-method.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value
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
  return p->get();
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

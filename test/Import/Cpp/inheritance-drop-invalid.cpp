// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/member-beside-base.cpp 2>&1 | FileCheck %s --check-prefix=MEMBERBASE
// RUN: not emitrust-import-c %t/member-of-derived.cpp 2>&1 | FileCheck %s --check-prefix=MEMBERDERIVED
// RUN: not emitrust-import-c %t/array-of-derived.cpp 2>&1 | FileCheck %s --check-prefix=ARRAYDERIVED
// RUN: not emitrust-import-c %t/global-of-derived.cpp 2>&1 | FileCheck %s --check-prefix=GLOBALDERIVED
// RUN: not emitrust-import-c %t/static-local-of-derived.cpp 2>&1 | FileCheck %s --check-prefix=STATICDERIVED
// RUN: not emitrust-import-c %t/byvalue-param-derived.cpp 2>&1 | FileCheck %s --check-prefix=BYVALPARAM
// RUN: not emitrust-import-c %t/byvalue-return-derived.cpp 2>&1 | FileCheck %s --check-prefix=BYVALRET
// RUN: not emitrust-import-c %t/value-copy-derived.cpp 2>&1 | FileCheck %s --check-prefix=VALUECOPY
// RUN: not emitrust-import-c %t/bare-block-derived.cpp 2>&1 | FileCheck %s --check-prefix=BAREBLOCK
// RUN: not emitrust-import-c %t/virtual-dtor-plus-method.cpp 2>&1 | FileCheck %s --check-prefix=MULTIVIRT
// RUN: not emitrust-import-c %t/empty-droppy-base-ctor.cpp 2>&1 | FileCheck %s --check-prefix=EMPTYCTOR
// RUN: not emitrust-import-c %t/empty-droppy-base-call.cpp 2>&1 | FileCheck %s --check-prefix=EMPTYHOP
// RUN: not emitrust-import-c %t/sizeof-polymorphic.cpp 2>&1 | FileCheck %s --check-prefix=SIZEOFPOLY

// W2.26 located-rejection ledger for the polymorphic-RAII value subset.
// The wave admits exactly TWO new class shapes -- a derived class over a
// destructor-carrying single base (with or without a destructor of its
// own, to any chain depth, empty droppy base included) and a class whose
// SOLE virtual member is its destructor -- because those are the shapes
// measured byte-identical against `clang++ -std=c++17`
// (inheritance-drop.cpp, cpp-inheritance-drop.cpp). Everything below
// stays a located rejection, now raised through the TRANSITIVE drop
// predicate: every W2.17 use-site gate keyed on `userDeclaredDestructor`,
// which answers FALSE for a merely-inheriting class, so before this wave
// none of these gates could see a droppy-DERIVED object at all -- the
// transitive predicate is a soundness prerequisite for the existing
// gates, not a nicety.
//
// The measured reason, one per case:
//
// * a droppy MEMBER beside a droppy BASE: the one drop-ORDER divergence
//   the spike measured -- C++ destroys members before the base subobject
//   (`~D ~M ~B`), Rust drops fields in declaration order and the base is
//   the FIRST field (`~D ~B ~M`). The W2.17 member gate stays, and goes
//   transitive with the rest.
// * a MEMBER whose class is droppy only by INHERITANCE: same forward-vs-
//   reverse member-order divergence as the directly-droppy member.
// * ARRAY / GLOBAL / STATIC LOCAL / BY-VALUE param+return / VALUE COPY /
//   BARE-BLOCK object of a droppy-DERIVED class: exactly W2.17's measured
//   divergences (reverse-vs-forward array order, never-dropped statics,
//   double-vs-single destructor run across a call boundary, copy lowered
//   to a move, flattened scope moving the drop point) -- each must fire
//   for a merely-inheriting class exactly as for a directly-droppy one.
// * a virtual destructor PLUS another virtual method: ADMITTED as a
//   VALUE since W2.19a, and since W2.19b the virtual call is admitted
//   even through a LOCAL pointer bound to exactly one object (the call
//   devirtualizes -- virtual-methods-devirt.cpp). The arm pins the
//   remaining dynamic residual: a virtual call through a pointer
//   PARAMETER, whose caller set is open -- the one channel left where
//   the vptr the emitted struct lacks could be observed (the vptr
//   LAYOUT residual stays screened at sizeof/alignof, below).
// * a NON-TRIVIAL constructor of an empty droppy base: the emitted image
//   default-initializes the zero-field base field, so a user ctor body
//   would be silently dropped. The trivial construction is elided, same
//   as the non-droppy empty base.
// * an inherited member reached THROUGH an empty droppy base: kept as the
//   conservative W2.18 rejection this wave (the hop screens key on the
//   AST's isEmpty(), which materialization does not change).
// * the droppy-base UPCAST (`B *p = &d;` for a non-virtual droppy
//   chain) was pinned here as UPCAST until FR-120 flipped it positive:
//   the pin moved FORWARD into the byte-diff oracle
//   test/EndToEnd/cpp-upcast-pointer.cpp (its RB/RD legs).
//   delete-through-base-pointer stays unreachable (new/delete are
//   themselves rejections). The POLYMORPHIC upcast (the former VUPCAST
//   arm) flipped positive in W2.19b: the single-object region fact
//   makes the static bind exact, so the pin moved FORWARD to
//   virtual-methods-devirt.cpp and the byte-diff oracle
//   test/EndToEnd/cpp-devirt-base-pointer.cpp; the residual fences for
//   pointers WITHOUT that fact are pinned in
//   virtual-methods-values-invalid.cpp.
// * sizeof/alignof of a POLYMORPHIC class: clang folds the vptr-carrying
//   native layout (16 for `V` below) while the emitted struct has no
//   vptr (4 bytes) -- the same promise-a-layout-Rust-never-keeps screen
//   as bit-fields and long double, closed HERE because admitting the
//   sole-virtual-dtor class is what makes the fold reachable.

//--- member-beside-base.cpp
extern "C" int printf(const char *, ...);
struct B {
  int x;
  B(int v) : x(v) {}
  ~B() { printf("~B %d\n", x); }
};
struct Mem {
  int m;
  Mem(int v) : m(v) {}
  ~Mem() { printf("~M %d\n", m); }
};
struct D : B {
  // MEMBERBASE: member-beside-base.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct member of a class with a destructor
  Mem m;
  D(int v) : B(v), m(v) {}
  ~D() { printf("~D\n"); }
};
int use(int n) { D d(n); return d.m.m; }

//--- member-of-derived.cpp
extern "C" int printf(const char *, ...);
struct B {
  int x;
  B(int v) : x(v) {}
  ~B() { printf("~B %d\n", x); }
};
struct D : B {
  int y;
  D(int v) : B(v), y(v) {}
};
struct Holder {
  // MEMBERDERIVED: member-of-derived.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct member of a class with a destructor
  D d;
  Holder(int v) : d(v) {}
};
int use(int n) { Holder h(n); return h.d.y; }

//--- array-of-derived.cpp
extern "C" int printf(const char *, ...);
struct B {
  int x;
  B() : x(0) {}
  ~B() { printf("~B %d\n", x); }
};
struct D : B {
  int y;
  D() : y(0) {}
};
int use(int n) {
  // ARRAYDERIVED: array-of-derived.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: array of a class with a destructor
  D a[2];
  a[0].y = n;
  return a[0].y;
}

//--- global-of-derived.cpp
extern "C" int printf(const char *, ...);
struct B {
  int x;
  B() : x(0) {}
  ~B() { printf("~B %d\n", x); }
};
struct D : B {
  int y;
  D() : y(0) {}
};
// GLOBALDERIVED: global-of-derived.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global or static object of a class with a destructor
D g;
int use(int n) { g.y = n; return g.y; }

//--- static-local-of-derived.cpp
extern "C" int printf(const char *, ...);
struct B {
  int x;
  B() : x(0) {}
  ~B() { printf("~B %d\n", x); }
};
struct D : B {
  int y;
  D() : y(0) {}
};
int use(int n) {
  // STATICDERIVED: static-local-of-derived.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global or static object of a class with a destructor
  static D s;
  s.y += n;
  return s.y;
}

//--- byvalue-param-derived.cpp
extern "C" int printf(const char *, ...);
struct B {
  int x;
  B(int v) : x(v) {}
  ~B() { printf("~B %d\n", x); }
};
struct D : B {
  int y;
  D(int v) : B(v), y(v) {}
};
// BYVALPARAM: byvalue-param-derived.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: class with a destructor passed or returned by value
int takes(D d) { return d.y; }
int use(int n) {
  D d(n);
  return takes(d);
}

//--- byvalue-return-derived.cpp
extern "C" int printf(const char *, ...);
struct B {
  int x;
  B(int v) : x(v) {}
  ~B() { printf("~B %d\n", x); }
};
struct D : B {
  int y;
  D(int v) : B(v), y(v) {}
};
// BYVALRET: byvalue-return-derived.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: class with a destructor passed or returned by value
D make(int n) { return D(n); }
int use(int n) { return make(n).y; }

//--- value-copy-derived.cpp
extern "C" int printf(const char *, ...);
struct B {
  int x;
  B(int v) : x(v) {}
  ~B() { printf("~B %d\n", x); }
};
struct D : B {
  int y;
  D(int v) : B(v), y(v) {}
};
// The reachable copy spelling rejects in `emitCXXConstructInit`, BEFORE
// the W2.17 value-copy gate in `emitRValue` (which also went transitive,
// as defense in depth for any rvalue-position copy that slips past the
// init and signature gates): copy-INITIALIZING a local is the copy shape
// an object of a droppy-derived class can actually spell, and it is a
// located rejection, measured -- not a silent move.
// VALUECOPY: value-copy-derived.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: copy/move construction
int use(int n) {
  D d(n);
  D d2 = d;
  return d2.y;
}

//--- bare-block-derived.cpp
extern "C" int printf(const char *, ...);
struct B {
  int x;
  B(int v) : x(v) {}
  ~B() { printf("~B %d\n", x); }
};
struct D : B {
  int y;
  D(int v) : B(v), y(v) {}
};
// BAREBLOCK: bare-block-derived.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: object of a class with a destructor outside a function, loop, or branch body
int use(int n) {
  int r = 0;
  {
    D d(n);
    r = d.y;
  }
  return r;
}

//--- virtual-dtor-plus-method.cpp
// W2.19a admits this class as a value and W2.19b admits the local
// single-object pointer call (it devirtualizes); the pin moved FORWARD
// again, to the pointer PARAMETER -- the caller set is open, so the
// pointee's dynamic type is genuinely unknown and the call stays a
// located rejection.
extern "C" int printf(const char *, ...);
struct S {
  int id;
  virtual ~S() { printf("~S %d\n", id); }
  virtual int get() const { return id; }
};
// MULTIVIRT: virtual-dtor-plus-method.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: virtual method call through a pointer
int use(S *p) {
  return p->get();
}

//--- empty-droppy-base-ctor.cpp
extern "C" int printf(const char *, ...);
struct Shape {
  Shape() { printf("shape ctor\n"); }
  ~Shape() { printf("~Shape\n"); }
};
struct Circle : Shape {
  int r;
  // EMPTYCTOR: empty-droppy-base-ctor.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: constructor of an empty base class
  Circle(int v) : Shape(), r(v) {}
};
int use(int n) {
  Circle c(n);
  return c.r;
}

//--- empty-droppy-base-call.cpp
extern "C" int printf(const char *, ...);
struct Shape {
  ~Shape() { printf("~Shape\n"); }
  int id() const { return 1; }
};
struct Circle : Shape {
  int r;
  Circle(int v) : r(v) {}
};
// EMPTYHOP: empty-droppy-base-call.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: inherited member of an empty base class
int use(int n) {
  Circle c(n);
  return c.id() + c.r;
}

//--- sizeof-polymorphic.cpp
extern "C" int printf(const char *, ...);
struct V {
  int id;
  V(int i) : id(i) {}
  virtual ~V() { printf("~V %d\n", id); }
};
int use(int n) {
  V v(n);
  // SIZEOFPOLY: sizeof-polymorphic.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: sizeof/alignof of a polymorphic class
  return (int)sizeof(V) + v.id;
}

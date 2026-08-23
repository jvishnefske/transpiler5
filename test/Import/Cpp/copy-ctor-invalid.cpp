// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/nrvo.cpp 2>&1 | FileCheck %s --check-prefix=NRVO
// RUN: not emitrust-import-c %t/move-ctor.cpp 2>&1 | FileCheck %s --check-prefix=MOVE
// RUN: not emitrust-import-c %t/nonconst-copy.cpp 2>&1 | FileCheck %s --check-prefix=NONCONST
// RUN: not emitrust-import-c %t/copy-array.cpp 2>&1 | FileCheck %s --check-prefix=ARRAY
// RUN: not emitrust-import-c %t/self-copy.cpp 2>&1 | FileCheck %s --check-prefix=SELFCOPY
// RUN: not emitrust-import-c %t/droppy-byvalue.cpp 2>&1 | FileCheck %s --check-prefix=DROPPYBYVAL
// RUN: not emitrust-import-c %t/member-copy.cpp 2>&1 | FileCheck %s --check-prefix=MEMBERCOPY
// RUN: not emitrust-import-c %t/user-assign.cpp 2>&1 | FileCheck %s --check-prefix=USERASSIGN
// RUN: not emitrust-import-c %t/nonscalar-assign.cpp 2>&1 | FileCheck %s --check-prefix=NONSCALAR
// RUN: not emitrust-import-c %t/value-assign.cpp 2>&1 | FileCheck %s --check-prefix=VALUEASSIGN

// W2.23 admitted the user-provided `T(const T&)` copy constructor (see
// copy-ctor.cpp); this file pins everything that MUST stay a located
// rejection, because each shape below has a measured or structural
// divergence no lowering in the subset can close:
//
// * The NRVO-candidate return is the ONE implementation-defined row of the
//   spike's elision table: a `return` of a single named local runs 0 copies
//   by default and 1 under -fno-elide-constructors, on BOTH clang++ and
//   g++, so NO emission is byte-diff-clean against every conforming
//   compiler. Clang's own `ReturnStmt::getNRVOCandidate()` is the oracle --
//   it is non-null on exactly this shape and null on the whole admitted
//   table (two-return functions, param returns, prvalue factories).
// * Move constructors: move counts are elision-dependent too, and the
//   subset has no xvalue/last-use model. The class-level wording is kept.
// * A copy ctor taking non-const `T&` is outside the FR-48 shared-borrow
//   image (the receiver already holds the &mut).
// * An ARRAY of a copy-ctor class: `[T; N]` repeat is rustc E0277 once
//   `Copy` leaves the derive -- the same boundary W2.17 drew for droppy
//   arrays, caught at the declaration instead of in rustc.
// * Self-copy `T b = b;` aliases the &mut receiver with the & source.
// * A copy+DTOR class BY VALUE keeps W2.17's signature gate: native
//   destroys the parameter temp at the end of the CALLER's full-expression
//   while the move-into-callee image drops it inside the callee -- measured
//   divergent with two calls in one expression (the spike's twocall probe).
//   Return-by-value of the same class IS admitted (the temp moves out).
// * The implicit copy ctor of an aggregate holding a copy-ctor member is
//   byte-identical in principle but requires SYNTHESIZING a constructor
//   the AST does not contain -- recorded and deferred, natural rejection.
// * A USER `operator=` stays omitted (FR-112) and its use sites reject
//   honestly; only the IMPLICIT copy-assignment lowers memberwise, and only
//   over scalar fields.

//--- nrvo.cpp
struct T {
  int v;
  T(int x) : v(x) {}
  T(const T &o) : v(o.v) {}
};
T named(int x) {
  T t(x);
  // NRVO: nrvo.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: NRVO-candidate return of a class with a copy constructor
  return t;
}

//--- move-ctor.cpp
// A class declaring a move constructor stays class-level fatal, wholesale --
// even alongside an otherwise admissible copy ctor.
// MOVE: move-ctor.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: copy/move/delegating constructor
struct M {
  int v;
  M(int x) : v(x) {}
  M(const M &o) : v(o.v) {}
  M(M &&o) : v(o.v) {}
};
int use(int n) {
  M m(n);
  return m.v;
}

//--- nonconst-copy.cpp
// NONCONST: nonconst-copy.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: copy constructor taking a non-const reference
struct N {
  int v;
  N(int x) : v(x) {}
  N(N &o) : v(o.v) {}
};
int use(int n) {
  N m(n);
  return m.v;
}

//--- copy-array.cpp
struct A {
  int v;
  A() : v(0) {}
  A(const A &o) : v(o.v) {}
};
int use() {
  // ARRAY: copy-array.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: array of a class with a copy constructor
  A arr[2];
  return arr[0].v;
}

//--- self-copy.cpp
struct S {
  int v;
  S(int x) : v(x) {}
  S(const S &o) : v(o.v) {}
};
int use(int n) {
  // SELFCOPY: self-copy.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable reference argument and method receiver
  S b = b;
  return b.v;
}

//--- droppy-byvalue.cpp
extern "C" int printf(const char *, ...);
struct L {
  int id;
  L(int i) : id(i) {}
  L(const L &o) : id(o.id) { printf("copy\n"); }
  ~L() { printf("dtor %d\n", id); }
};
// DROPPYBYVAL: droppy-byvalue.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: class with a destructor passed or returned by value
int take(L x) { return x.id; }
int use(int n) {
  L a(n);
  return take(a);
}

//--- member-copy.cpp
struct Inner {
  int v;
  Inner(int x) : v(x) {}
  Inner(const Inner &o) : v(o.v) {}
};
struct Outer {
  Inner i;
  Outer(int x) : i(x) {}
};
int use(int n) {
  Outer a(n);
  // MEMBERCOPY: member-copy.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: copy/move construction
  Outer b = a;
  return b.i.v;
}

//--- user-assign.cpp
struct U {
  int v;
  U &operator=(const U &o) {
    v = o.v + 1;
    return *this;
  }
};
int use(int n) {
  U a, b;
  a.v = n;
  b.v = 0;
  // USERASSIGN: user-assign.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to overloaded operator 'operator=' omitted from class 'U'
  b = a;
  return b.v;
}

//--- nonscalar-assign.cpp
// The memberwise image covers scalar fields only: a RECORD member would need
// the member's own assignment semantics recursively, which is exactly the
// shape FR-112's Inner-with-user-operator= channel arrives through.
struct P {
  int x;
  int y;
};
struct Holder {
  P p;
  int n;
};
int use(int n) {
  Holder a, b;
  a.n = n;
  a.p.x = 1;
  a.p.y = 2;
  b.n = 0;
  // NONSCALAR: nonscalar-assign.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: implicit copy assignment of a class with non-scalar members
  b = a;
  return b.n + b.p.x;
}

//--- value-assign.cpp
// The memberwise lowering is statement-position ONLY: the operator's `T&`
// result has no representation, so a VALUE use keeps a located rejection --
// with the honest wording, not the pre-W2.23 "omitted from class" message.
struct T {
  int v;
};
int take(T x) { return x.v; }
int use(int n) {
  T a, b;
  a.v = n;
  // VALUEASSIGN: value-assign.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: implicit copy assignment in value position
  return take(b = a);
}

// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/op-call.cpp 2>&1 | FileCheck %s --check-prefix=OPCALL
// RUN: not emitrust-import-c %t/member-assign.cpp 2>&1 | FileCheck %s --check-prefix=MEMASSIGN
// RUN: not emitrust-import-c %t/implicit-assign.cpp 2>&1 | FileCheck %s --check-prefix=IMPLASSIGN
// RUN: not emitrust-import-c %t/explicit-spelling.cpp 2>&1 | FileCheck %s --check-prefix=EXPLICIT
// RUN: not emitrust-import-c %t/body-fail-use.cpp 2>&1 | FileCheck %s --check-prefix=BODYUSE
// RUN: not emitrust-import-c %t/static-fail-use.cpp 2>&1 | FileCheck %s --check-prefix=STATICUSE
// RUN: not emitrust-import-c %t/copy-ctor.cpp 2>&1 | FileCheck %s --check-prefix=COPYCTOR

// FR-112 located-rejection ledger for USES of an OMITTED MEMBER. Containment
// (cpp-contained-member.cpp) keeps a class importable when a member-level
// shape -- an overloaded operator, or a method whose signature or body fails
// to import -- would previously have rejected the whole class. That is only
// sound under the criterion design.md's FR-112 entry derives: an omitted
// member is safe iff every use of it appears in the AST as an EXPLICIT node
// the importer's expression walk visits, so nothing omitted can ever be
// invoked silently. This file is that guarantee, one section per use
// channel.
//
// `op-call` and `member-assign` are the spelled operator uses; the wording
// names the omitted member AND its class so the rejection is rankable
// (ledger tag `cxx-omitted-member`, mirrored in
// lib/ImportC/RejectionLedger.cpp and test/RealWorld/run_realworld.py)
// instead of the pre-FR-112 bare "unsupported callee" that tabulated as
// `other`.
//
// `implicit-assign` is the nastiest channel, and it was already closed
// before this wave: a user `operator=` reached through an ENCLOSING class's
// IMPLICIT copy-assignment (`b = a` on a class whose MEMBER declares the
// operator) still surfaces as a `CXXOperatorCallExpr` naming the enclosing
// class's implicit `operator=` -- an explicit AST node at the assignment,
// caught by the same guard.
//
// `body-fail-use` / `static-fail-use` are the containment channels proper: a
// method whose body fails to import is omitted with its own per-construct
// diagnostic (a warning, since the class still imports), and the call site
// takes `call to unimported method '<mangled>'`. The static variant matters
// separately because a resolved static call renders as an opaque callee
// string the verifier never checks (design.md FR-112 constraint C8), so the
// IMPORT-level rejection pinned here is the only tier that catches it before
// rustc.
//
// `copy-ctor` pins the boundary of containment itself: a copy/move/
// delegating constructor is invoked IMPLICITLY at by-value pass, return and
// init -- there is NO CALL NODE to reject -- so omitting one would
// substitute Rust's bitwise Copy for the user's constructor. On exactly this
// input the native binary prints `1 99`; a contained import would print
// `1 1`. It must NEVER import, neither wholesale nor contained, which is why
// the class-level gate stays.

//--- op-call.cpp
// OPCALL: op-call.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to overloaded operator 'operator+' omitted from class 'Vec2'
struct Vec2 {
  int x;
  int operator+(const Vec2 &o) const { return x + o.x; }
};
int use(int n) {
  Vec2 a, b;
  a.x = n;
  b.x = n + 1;
  return a + b;
}

//--- member-assign.cpp
// MEMASSIGN: member-assign.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to overloaded operator 'operator=' omitted from class 'Inner'
struct Inner {
  int v;
  Inner &operator=(const Inner &o) {
    v = o.v + 1;
    return *this;
  }
};
int use(int n) {
  Inner a, b;
  a.v = n;
  b.v = 0;
  b = a;
  return b.v;
}

//--- implicit-assign.cpp
// IMPLASSIGN: implicit-assign.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to overloaded operator 'operator=' omitted from class 'Outer'
struct Inner {
  int v;
  Inner &operator=(const Inner &o) {
    v = o.v + 1;
    return *this;
  }
};
struct Outer {
  Inner i;
  int w;
};
int use(int n) {
  Outer a, b;
  a.i.v = n;
  a.w = 1;
  b.w = 2;
  b = a;
  return b.i.v + b.w;
}

//--- explicit-spelling.cpp
// The explicit member-call spelling of an operator keeps FR-117's wording
// (raised in the member-call dispatch, ahead of the mangled lookup that
// would otherwise leak the empty-base-name symbol `'Vec2_'`).
// EXPLICIT: explicit-spelling.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: overloaded operator
struct Vec2 {
  int x;
  int operator+(const Vec2 &o) const { return x + o.x; }
};
int use(int n) {
  Vec2 a, b;
  a.x = n;
  b.x = n + 1;
  return a.operator+(b);
}

//--- body-fail-use.cpp
// The omission itself is a WARNING carrying the method's own per-construct
// diagnostic (the class imports); the USE is the error.
// BODYUSE: body-fail-use.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported pointer expression: CStyleCastExpr (omitted: method 'bad' of class 'C')
// BODYUSE: body-fail-use.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to unimported method 'C_bad'
struct C {
  int v;
  int bad() const { return *(int *)(long)v; }
  int good() const { return v + 1; }
};
int use(int n) {
  C c;
  c.v = n;
  return c.bad() + c.good();
}

//--- static-fail-use.cpp
// STATICUSE: static-fail-use.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported pointer expression: CStyleCastExpr (omitted: method 'helper' of class 'Calc')
// STATICUSE: static-fail-use.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to unimported method 'Calc_helper'
struct Calc {
  static int helper(int n) { return *(int *)(long)n; }
  int v;
};
int use(int n) { return Calc::helper(n); }

//--- copy-ctor.cpp
// COPYCTOR: copy-ctor.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: copy/move/delegating constructor
extern "C" int printf(const char *, ...);
struct P {
  int v;
  P() : v(1) {}
  P(const P &o) : v(99) {}
};
int main() {
  P a;
  P b = a;
  printf("%d %d\n", a.v, b.v);
  return 0;
}

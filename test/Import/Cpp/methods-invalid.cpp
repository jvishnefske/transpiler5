// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/destructor.cpp 2>&1 | FileCheck %s --check-prefix=DTOR
// RUN: not emitrust-import-c %t/virtual.cpp 2>&1 | FileCheck %s --check-prefix=VIRTUAL
// RUN: not emitrust-import-c %t/try-catch.cpp 2>&1 | FileCheck %s --check-prefix=TRYCATCH
// RUN: not emitrust-import-c %t/throw.cpp 2>&1 | FileCheck %s --check-prefix=THROW
// RUN: not emitrust-import-c %t/operator.cpp 2>&1 | FileCheck %s --check-prefix=OPERATOR
// RUN: not emitrust-import-c %t/template.cpp 2>&1 | FileCheck %s --check-prefix=TEMPLATE
// RUN: not emitrust-import-c %t/refmember.cpp 2>&1 | FileCheck %s --check-prefix=REFMEMBER

// W2.2 located-rejection baseline for the class-methods subset. Three of
// the seven cases below are NEW pins this wave (destructor, virtual,
// operator-overload): today, with methods still silently skipped
// (W2.0/W2.1's "a method on a class with no base classes is silently
// IGNORED" baseline), none of these three produce the wording pinned
// here — the destructor case currently produces NO diagnostic at all
// (the whole file imports cleanly, so `not` itself fails), and the
// virtual/operator cases currently fail later and differently (a generic
// "call to unimported function" / "unsupported callee" at the call site,
// not a located rejection at the declaration). The other four
// (try/catch, throw, template class, reference-typed member) are
// EXISTING W2.0-era rejections, verified unchanged in a class-method
// context; they are pinned here too for a complete, one-file located-
// rejection ledger for this subset, per the wave's deliverable list.

//--- destructor.cpp
// A user-declared destructor is out of scope this wave (no drop
// semantics): REJECT at the destructor's own declaration, regardless of
// whether it is ever invoked.
// DTOR: destructor.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: user-declared destructor
class HasDtor {
public:
  ~HasDtor() {}
  int inc(int d) { return x + d; }
  int x;
};

int use(void) {
  HasDtor h;
  h.x = 1;
  return h.inc(h.x);
}

//--- virtual.cpp
// A virtual method is out of scope this wave (no vtable/dynamic dispatch):
// REJECT at the method's own declaration.
// VIRTUAL: virtual.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: virtual method
class Base2 {
public:
  virtual int f() { return 1; }
};

int use(Base2 *b) {
  return b->f();
}

//--- try-catch.cpp
// Existing W2.0 rejection (test/Import/Cpp/cpp-basics-invalid.cpp's
// EXCEPTION case), re-verified unchanged when the try/catch lives inside
// a method body rather than a free function.
// TRYCATCH: try-catch.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported statement: CXXTryStmt
class Risky {
public:
  int attempt(int x) {
    try {
      return x;
    } catch (...) {
      return 0;
    }
  }
};

int use(void) {
  Risky r;
  return r.attempt(5);
}

//--- throw.cpp
// Existing W2.0-era rejection (CXXThrowExpr falls through the expression
// dispatch like any other unrecognized Expr kind), re-verified unchanged
// inside a method body.
// THROW: throw.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported expression: CXXThrowExpr
class Thrower {
public:
  int risky(int x) {
    if (x < 0) {
      throw x;
    }
    return x;
  }
};

int use(void) {
  Thrower t;
  return t.risky(5);
}

//--- operator.cpp
// Operator overloading beyond none is out of scope this wave: REJECT at
// the operator method's own declaration. Today (NEW pin not yet wired),
// this instead fails later, differently, and less precisely at the call
// site with "unsupported callee".
// OPERATOR: operator.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: overloaded operator
struct Vec2 {
  int x;
  int operator+(const Vec2 &o) const { return x + o.x; }
};

int use(void) {
  Vec2 a;
  a.x = 1;
  Vec2 b;
  b.x = 2;
  return a + b;
}

//--- template.cpp
// Existing W2.0-era rejection: a class template falls through the
// top-level decl dispatch exactly like the free-function template case
// already pinned in cpp-basics-invalid.cpp's TEMPLATE check, re-verified
// unchanged for a template CLASS with a member function.
// TEMPLATE: template.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported top-level declaration
template <typename T>
class Box {
public:
  T value;
  T get() { return value; }
};

int use(void) {
  Box<int> b;
  b.value = 5;
  return b.get();
}

//--- refmember.cpp
// Existing W2.0-era rejection: a reference-typed data member ("references
// as members" this wave's OUT list) is rejected exactly like a
// reference-typed function parameter, at the member declaration, whether
// or not a method ever touches it.
// REFMEMBER: refmember.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference types are not yet supported
int global_val = 3;

struct Holder {
  int &r;
  int get() { return r; }
};

int use(void) {
  Holder h{global_val};
  return h.get();
}

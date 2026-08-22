// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/virtual.cpp 2>&1 | FileCheck %s --check-prefix=VIRTUAL
// RUN: not emitrust-import-c %t/try-catch.cpp 2>&1 | FileCheck %s --check-prefix=TRYCATCH
// RUN: not emitrust-import-c %t/throw.cpp 2>&1 | FileCheck %s --check-prefix=THROW
// RUN: not emitrust-import-c %t/operator.cpp 2>&1 | FileCheck %s --check-prefix=OPERATOR
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
//
// W2.17 RETIRED this file's DESTRUCTOR case. `//--- destructor.cpp` used
// to pin `class HasDtor { ~HasDtor() {} int inc(int d); int x; };` plus a
// local of it as `error: unsupported: user-declared destructor`; a
// non-virtual destructor defined in this translation unit is now ADMITTED
// and lowers to `impl Drop for HasDtor`, so that exact input IMPORTS. The
// pin moved FORWARD rather than loosening: the positive behaviour is
// pinned in test/Import/Cpp/destructors.cpp (and byte-diffed in
// test/EndToEnd/cpp-destructor.cpp), and the surviving destructor frontier
// -- virtual destructors, a destructor with no definition in this TU, a
// union destructor, and the six OBJECT positions whose drop point the
// emitter cannot reproduce (member, array, global/static, by value, an
// unmodeled scope, a side-effecting for-increment) -- in
// test/Import/Cpp/destructors-invalid.cpp. The generic wording this case
// pinned is still raised, for the union shape.
//
// FR-112 MOVED this file's OPERATOR, TRYCATCH and THROW pins from the
// class to the USE SITE. A member-level shape -- an overloaded operator,
// or a method whose body fails to import (try/catch and throw are body
// failures here) -- no longer rejects the whole class: the member is
// OMITTED (with the method's own per-construct diagnostic as a warning,
// since the class still imports), the siblings import, and every USE of
// the omitted member is a located error. The pins moved FORWARD rather
// than loosening: the positive half (class imports with the member
// omitted) is in test/Import/Cpp/cpp-contained-member.cpp, the per-channel
// use-site rejections in cpp-contained-member-invalid.cpp, and the
// caller-before-callee fixpoint in cpp-contained-fixpoint.cpp. VIRTUAL
// stays a class-level rejection this wave: the vptr is real storage the
// emitted struct lacks (`sizeof` folds faithfully from clang for a struct
// the emitter renders smaller), which no use-site rejection can repair.
//
// W2.16 RETIRED this file's TEMPLATE case. `//--- template.cpp` used to
// pin `template <typename T> class Box { public: T value; T get(); };`
// plus `Box<int> b;` as `error: unsupported top-level declaration`;
// class-template monomorphization landed, so that exact input now
// IMPORTS, to `emitrust.struct_def @Box_i32` with a `Box_i32_get` method
// riding this very wave's `importCXXMethods` surface unchanged. The pin
// moved FORWARD rather than loosening: the positive behaviour (and the
// binding struct-name suffix scheme) is pinned in
// test/Import/Cpp/class-templates.cpp, and the surviving class-template
// frontier — explicit and partial specializations, non-type template
// arguments, parameter packs, member function templates, static data
// members, and same-TU name clashes — in
// test/Import/Cpp/class-templates-invalid.cpp.

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
// EXCEPTION case). FR-112: inside a METHOD body it is now a containment
// omission (warning, class imports) and the member's USE is the error.
// TRYCATCH: try-catch.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported statement: CXXTryStmt (omitted: method 'attempt' of class 'Risky')
// TRYCATCH: try-catch.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to unimported method 'Risky_attempt'
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
// dispatch like any other unrecognized Expr kind). FR-112: inside a
// method body it is a containment omission; the use is the error.
// THROW: throw.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported expression: CXXThrowExpr (omitted: method 'risky' of class 'Thrower')
// THROW: throw.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to unimported method 'Thrower_risky'
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
// FR-112 flipped this pin BACK to the call site, but located and named
// this time: the operator is OMITTED from the class (which imports, with
// its siblings intact -- cpp-contained-member.cpp), and the spelled use
// rejects with a wording that names the omitted member and its class,
// rather than W2.2's class-level rejection or the pre-W2.2 bare
// "unsupported callee".
// OPERATOR: operator.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to overloaded operator 'operator+' omitted from class 'Vec2'
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

//--- refmember.cpp
// A reference-typed data member is rejected at the member declaration,
// whether or not a method ever touches it. FR-48 landed reference
// PARAMETERS but deliberately did NOT land members, and the message was
// sharpened to name the position: the difficulty here is ownership rather
// than syntax -- the borrow outlives the expression that created it and is
// stored in an object whose lifetime is unrelated to the referent's, so
// the emitted struct would need a lifetime parameter nothing can infer.
// REFMEMBER: refmember.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference struct members are not yet supported
int global_val = 3;

struct Holder {
  int &r;
  int get() { return r; }
};

int use(void) {
  Holder h{global_val};
  return h.get();
}

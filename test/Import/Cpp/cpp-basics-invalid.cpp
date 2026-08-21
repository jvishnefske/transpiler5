// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/base-class.cpp 2>&1 | FileCheck %s --check-prefix=BASECLASS
// RUN: not emitrust-import-c %t/reference-return.cpp 2>&1 | FileCheck %s --check-prefix=REFRETURN
// RUN: not emitrust-import-c %t/try-catch.cpp 2>&1 | FileCheck %s --check-prefix=EXCEPTION

// W2.0 AST-tolerance baseline. Two constructs get a dedicated located
// rejection this wave (base classes, references); one more is
// already trivially rejected via an EXISTING generic diagnostic that
// predates W2.0 (a try/catch statement falls through the statement
// dispatch exactly like any other unrecognized Stmt kind) — pinned here
// so a later wave (member functions, exceptions) has a documented
// baseline to work from.
//
// W2.15 RETIRED this file's third case. `//--- template.cpp` used to pin
// `template <typename T> T identity(T x)` plus `identity<int>(41)` as
// `error: unsupported top-level declaration`; function-template
// monomorphization landed, so that exact input now IMPORTS, to
// `func.func @identity_i32`. The pin moved forward rather than
// loosening: the positive behaviour (and the binding template-argument
// suffix scheme) is pinned in test/Import/Cpp/function-templates.cpp and
// the surviving template frontier — explicit specializations, non-type
// template arguments, parameter packs — in
// test/Import/Cpp/function-templates-invalid.cpp. W2.16 then did the
// same for a template CLASS — `template <typename T> struct Box` now
// imports one struct per instantiation (`Box<int>` -> `Box_i32`), pinned
// in test/Import/Cpp/class-templates.cpp, with its own surviving
// frontier in test/Import/Cpp/class-templates-invalid.cpp. Neither
// spelling reaches the generic top-level wording any more.
//
// Virtual methods and multiple/virtual inheritance
// are deliberately NOT covered here: a method (virtual or not) on a class
// with no base classes is silently IGNORED, not rejected, by design this
// wave (see cpp-basics.cpp and ImportC.cpp's file comment) — there is no
// diagnostic to pin for that shape yet.

//--- base-class.cpp
struct Base {
  int x;
};

// BASECLASS: base-class.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: base classes are not supported
struct Derived : Base {
  int y;
};

int use(void) {
  Derived d;
  d.y = 1;
  return d.y;
}

//--- reference-return.cpp
// FR-48 landed reference PARAMETERS, so the shape this case used to pin
// (`int add_one(int &x)`) now imports as `&mut i32` and is pinned as an
// ACCEPT in cpp-references.cpp instead. A reference RETURN is the residual
// this slot now guards: returning a borrow means naming the lifetime it
// stays valid for, and nothing in the signature distinguishes a referent
// that outlives the call from a callee local, so it keeps a located
// rejection rather than an elided-lifetime guess.
// REFRETURN: reference-return.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference return types are not yet supported
int &pick(int &x) {
  return x;
}

int use(void) {
  int v = 41;
  return pick(v);
}

//--- try-catch.cpp
// EXCEPTION: try-catch.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported statement: CXXTryStmt
int risky(void) {
  try {
    return 41;
  } catch (...) {
    return 0;
  }
}

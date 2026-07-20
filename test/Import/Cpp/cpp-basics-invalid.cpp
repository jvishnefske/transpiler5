// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/base-class.cpp 2>&1 | FileCheck %s --check-prefix=BASECLASS
// RUN: not emitrust-import-c %t/reference-param.cpp 2>&1 | FileCheck %s --check-prefix=REFPARAM
// RUN: not emitrust-import-c %t/template.cpp 2>&1 | FileCheck %s --check-prefix=TEMPLATE
// RUN: not emitrust-import-c %t/try-catch.cpp 2>&1 | FileCheck %s --check-prefix=EXCEPTION

// W2.0 AST-tolerance baseline. Two constructs get a dedicated located
// rejection this wave (base classes, reference parameters); two more are
// already trivially rejected via EXISTING generic diagnostics that predate
// W2.0 (class templates fall through the top-level decl dispatch exactly
// like any other unrecognized Decl kind; a try/catch statement falls
// through the statement dispatch the same way) — pinned here so a later
// wave (templates, member functions, exceptions) has a documented
// baseline to work from. Virtual methods and multiple/virtual inheritance
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

//--- reference-param.cpp
// REFPARAM: reference-param.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference types are not yet supported
int add_one(int &x) {
  return x + 1;
}

int use(void) {
  int v = 41;
  return add_one(v);
}

//--- template.cpp
// TEMPLATE: template.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported top-level declaration
template <typename T>
T identity(T x) {
  return x;
}

int use(void) {
  return identity<int>(41);
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

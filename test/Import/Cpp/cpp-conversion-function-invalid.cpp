// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/implicit-use.cpp 2>&1 | FileCheck %s --check-prefix=IMPLICIT
// RUN: not emitrust-import-c %t/static-cast-use.cpp 2>&1 | FileCheck %s --check-prefix=STATICCAST
// RUN: not emitrust-import-c %t/explicit-call.cpp 2>&1 | FileCheck %s --check-prefix=EXPLICIT
// RUN: not emitrust-import-c %t/out-of-line.cpp 2>&1 | FileCheck %s --check-prefix=OUTOFLINE
// RUN: not emitrust-import-c %t/union-operator.cpp 2>&1 | FileCheck %s --check-prefix=UNIONOP

// FR-117 located-rejection ledger for the non-identifier `DeclarationName`
// family: conversion functions and overloaded operators. A method whose name
// is not an ordinary identifier has NO spelling to mangle, and on unpatched
// HEAD every one of them silently mangled to the EMPTY string.
//
// Conversion functions are OMITTED from the imported class (pinned positive
// in cpp-conversion-function.cpp), which is only sound if every USE of one
// is loud. The first three sections are that guarantee -- the two implicit
// channels were already located rejections, and the third, the explicit
// `c.operator int()` call spelling, is a NEW rejection this wave adds. It
// previously WORKED (it lowered to a correct call to the empty-named
// method), so this pin records a deliberate FRONTIER MOVE: the working
// spelling is withdrawn in exchange for the class importing at all, and the
// diagnostic must not leak the empty mangled name (`'C_'`) that the old
// path would have produced.
//
// `out-of-line` is the backstop: an out-of-line conversion-function
// DEFINITION is a top-level item that reaches `importFunction` directly, so
// omitting it from `importCXXMethods` alone would leave it emitting an
// unnamed symbol. Per the repo rule that a recovered item reaching emission
// unresolved must fail loudly, `importFunction` refuses it.
//
// `union-operator` closes a bypass that is broader than design.md's FR-117
// entry recorded: the C++ UNION path never calls `collectRecordFields`
// (`importRecordUncached` takes the `collectUnionSlot` branch), so the W2.2
// member-shape gate never ran on a union and a union's overloaded operator
// was ADMITTED as another empty-named method -- while FR-41's coloring probe
// screened the same union Red. That was a FALSE RED, the one direction
// ItemColoring's doctrine forbids. The union path now raises the SAME
// wording the struct path always did, so the screen and the importer agree.
//
// A fourth channel on the same axis is KNOWN AND DELIBERATELY LEFT OPEN
// here: a NON-MEMBER `operator+` is an ordinary top-level `FunctionDecl`
// that never goes near any record gate, and measured on this build it still
// imports as `emitrust.func @<<INVALID EMPTY SYMBOL>>` and emits the
// literally unparseable Rust `pub fn (v0: A, b: i32) -> i32`. It is not
// fixed in this wave because every free operator the suite exercises is
// already fenced by a MORE SPECIFIC diagnostic raised later on the same
// item (see the `user-operator` case in ostream-invalid.cpp and `struct-key`
// in stl-map-invalid.cpp), and a blanket refusal here would replace those
// pins with a vaguer wording; a real fix needs a symbol spelling for a
// non-identifier name, which is a naming change with byte-identity
// consequences. See lib/ImportC/ImportCFunctions.cpp's `importFunction`
// prologue.

//--- implicit-use.cpp
// IMPLICIT: implicit-use.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported cast (UserDefinedConversion)
struct C {
  int v;
  operator int() const { return v; }
};

int use(int n) {
  C c;
  c.v = n;
  int x = c;
  return x;
}

//--- static-cast-use.cpp
// STATICCAST: static-cast-use.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported cast (UserDefinedConversion)
struct C {
  int v;
  operator int() const { return v; }
};

int use(int n) {
  C c;
  c.v = n;
  return static_cast<int>(c);
}

//--- explicit-call.cpp
// EXPLICIT: explicit-call.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conversion function
struct C {
  int v;
  operator int() const { return v; }
};

int use(int n) {
  C c;
  c.v = n;
  return c.operator int();
}

//--- out-of-line.cpp
// OUTOFLINE: out-of-line.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conversion function
struct C {
  int v;
  operator int() const;
};

C::operator int() const { return v; }

//--- union-operator.cpp
// UNIONOP: union-operator.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: overloaded operator
union U {
  int a;
  float b;
  int operator+(int x) const { return a + x; }
};

int use(int n) {
  U u;
  u.a = n;
  return u.a;
}

// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/conv.c 2>&1 | FileCheck %s --check-prefix=CONV
// RUN: not emitrust-import-c %t/keyword.c 2>&1 | FileCheck %s --check-prefix=KEYWORD
// RUN: not emitrust-import-c %t/distinct.c 2>&1 | FileCheck %s --check-prefix=DISTINCT
// RUN: not emitrust-import-c %t/big.c 2>&1 | FileCheck %s --check-prefix=BIG
// RUN: not emitrust-import-c %t/global-init.c 2>&1 | FileCheck %s --check-prefix=GLOBALINIT
// RUN: not emitrust-import-c %t/shape1.c %t/shape2.c 2>&1 | FileCheck %s --check-prefix=SHAPE

// Integer-to-enum conversions are value-preserving (open enums), but a
// floating value converted directly to an enum destination has no integer
// intermediary in the AST and stays rejected; enumerator spellings are
// emitted verbatim as Rust identifiers, so a spelling that is a Rust
// keyword is rejected; and two distinct enum types map to distinct Rust
// types with no common comparison, so comparing them is rejected. All
// diagnostics carry the file:line:col location. FR-113 (which admitted
// scoped enums) pins the rest of the stays-rejected list here in the same
// style: an enumerator value outside the i32 range, an enum-typed global
// with an enumerator initializer, and a cross-TU redefinition with a
// different shape.

// CONV: conv.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: floating to enum conversion
// KEYWORD: keyword.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enumerator 'match' is a Rust keyword
// DISTINCT: distinct.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: comparison between distinct enum types
// BIG: big.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enumerator value does not fit in i32
// GLOBALINIT: global-init.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global initializer for this type
// SHAPE: shape2.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of enum 'E' with a different shape in another translation unit

//--- conv.c
enum Mode { Off, On };

int set(double raw) {
  enum Mode m = Off;
  m = (enum Mode)raw;
  return m == On;
}

//--- keyword.c
enum Token { match, other };

int first(void) {
  return match;
}

//--- distinct.c
enum A { Left };
enum B { Right };

int same(enum A a, enum B b) {
  return a == b;
}

//--- big.c
enum Big { HUGE_V = 3000000000 };

int f(void) {
  return 0;
}

//--- global-init.c
enum Color { RED, GREEN };
enum Color g = GREEN;

int use(void) {
  return (int)g;
}

//--- shape1.c
enum E { A = 1 };

int f(void) {
  return A;
}

//--- shape2.c
enum E { A = 2 };

int g(void) {
  return A;
}

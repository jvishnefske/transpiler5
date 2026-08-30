// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/conv.c 2>&1 | FileCheck %s --check-prefix=CONV
// RUN: not emitrust-import-c %t/keyword.c 2>&1 | FileCheck %s --check-prefix=KEYWORD
// RUN: not emitrust-import-c %t/distinct.c 2>&1 | FileCheck %s --check-prefix=DISTINCT
// RUN: not emitrust-import-c %t/big.c 2>&1 | FileCheck %s --check-prefix=BIG
// RUN: not emitrust-import-c %t/u32max.c 2>&1 | FileCheck %s --check-prefix=U32MAX
// RUN: not emitrust-import-c %t/unsat.c 2>&1 | FileCheck %s --check-prefix=UNSAT
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
// style: an enumerator value outside the enum's underlying-type range, an
// enum-typed global with an enumerator initializer, and a cross-TU
// redefinition with a different shape.
//
// FR-166 admitted 64-BIT underlying types (test/EndToEnd/enum-wide-
// underlying.c), which splits the range gate in two, and BOTH halves are
// pinned here because each guards a different failure:
//   * BIG / U32MAX -- clang's underlying type is 32 bits wide, so the value
//     must fit i32. The u32 half is NOT an oversight: `castEnumToI32`
//     discards signedness at every one of its call sites, so admitting
//     `M_MAX = 4294967295u` would emit `4294967295 as i32` == -1 and
//     MISCOMPILE every comparison against it (native `cmp 1 1 1` vs rust
//     `cmp 1 0 1`). Widening this gate is FR-169's job, and it must fix the
//     signedness first -- do not "tidy" this leg away.
//   * UNSAT -- the underlying type is 64 bits wide and admitted, but the
//     value is above INT64_MAX and `DenseI64ArrayAttr` cannot represent it,
//     so the enum stays a LOCATED rejection rather than silently wrapping.

// CONV: conv.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: floating to enum conversion
// KEYWORD: keyword.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enumerator 'match' is a Rust keyword
// DISTINCT: distinct.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: comparison between distinct enum types
// BIG: big.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enumerator value does not fit in i32
// U32MAX: u32max.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enumerator value does not fit in i32
// UNSAT: unsat.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enumerator value does not fit in i64
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
// 3000000000 fits `unsigned int`, so clang picks a 32-BIT underlying type
// and the i32 range gate still applies.
enum Big { HUGE_V = 3000000000 };

int f(void) {
  return 0;
}

//--- u32max.c
// UINT32_MAX likewise keeps a 32-bit underlying type. Admitting it without
// first teaching the enum-to-integer conversion about signedness would be a
// miscompile, not a feature (see the header comment).
enum M { M_A = 1, M_MAX = 0xFFFFFFFFU };

int use(void) {
  enum M m = M_MAX;
  return m == M_MAX;
}

//--- unsat.c
// A 64-bit underlying type IS admitted by FR-166, but a value above
// INT64_MAX has no `DenseI64ArrayAttr` representation and stays rejected.
enum U { U_A = 1, U_BIG = 0xFFFFFFFFFFFFFFFFULL };

int big(void) {
  enum U u = U_BIG;
  return (int)u;
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

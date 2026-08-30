// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/conv.c 2>&1 | FileCheck %s --check-prefix=CONV
// RUN: not emitrust-import-c %t/keyword.c 2>&1 | FileCheck %s --check-prefix=KEYWORD
// RUN: not emitrust-import-c %t/distinct.c 2>&1 | FileCheck %s --check-prefix=DISTINCT
// RUN: emitrust-import-c %t/big.c | FileCheck %s --check-prefix=BIG
// RUN: emitrust-import-c %t/u32max.c | FileCheck %s --check-prefix=U32MAX
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
// underlying.c), which splits the range gate in two. FR-166 PHASE 2 then
// MOVED THE 32-BIT HALF FORWARD, so the two legs that used to pin it --
// BIG and U32MAX -- are kept here as POSITIVE checks rather than deleted,
// because where a pin moved to is as much a fact as where it was:
//   * BIG / U32MAX -- clang's underlying type is 32 bits wide and UNSIGNED
//     (no enumerator is negative), so the admitted range is the full u32,
//     not i32. Both import now. This was held back through the FR-166 wave
//     on purpose: while `castEnumToI32` sat on every enum-to-integer path,
//     an admitted `M_MAX = 4294967295u` became `4294967295 as i32` == -1
//     and MISCOMPILED every comparison and every widening against it
//     (measured: `(unsigned long)M_MAX` printed 18446744073709551615). The
//     range only opened together with FR-169 phases A and C, which is what
//     test/Import/C/enum-u32-enumerator.c and
//     test/EndToEnd/enum-u32-enumerator-range.c pin.
//     NOTE there is now no C program that reaches "does not fit in i32":
//     clang widens the underlying type to 64 bits rather than hand the
//     importer a 32-bit enum with an out-of-range enumerator, so the
//     surviving reachable range rejection is UNSAT alone. The i32 leg of
//     the gate survives for a SIGNED 32-bit underlying type and is pinned
//     at the dialect verifier instead (test/Dialect/EmitRust/invalid.mlir).
//   * UNSAT -- the underlying type is 64 bits wide and admitted, but the
//     value is above INT64_MAX and `DenseI64ArrayAttr` cannot represent it,
//     so the enum stays a LOCATED rejection rather than silently wrapping.
//     THAT bound does not move: it is a representation limit, not a policy.

// CONV: conv.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: floating to enum conversion
// KEYWORD: keyword.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enumerator 'match' is a Rust keyword
// DISTINCT: distinct.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: comparison between distinct enum types
// BIG: emitrust.enum_def @Big ["HUGE_V"] [3000000000] {unsigned_underlying}
// U32MAX: emitrust.enum_def @M ["M_A", "M_MAX"] [1, 4294967295] {unsigned_underlying}
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
// 3000000000 fits `unsigned int`, so clang picks a 32-bit UNSIGNED
// underlying type -- and FR-166 phase 2 admits the whole u32 range for one.
enum Big { HUGE_V = 3000000000 };

int f(void) {
  return 0;
}

//--- u32max.c
// UINT32_MAX likewise keeps a 32-bit unsigned underlying type, and is the
// top of the range FR-166 phase 2 opened. Admitting it without first
// teaching the enum-to-integer conversions about signedness WAS a
// miscompile (see the header comment); FR-169 phases A and C are what
// make it a feature.
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

// RUN: emitrust-import-c %s | FileCheck %s --implicit-check-not="@C_(" \
// RUN:   --implicit-check-not="@C_i(" --implicit-check-not="@C_l("
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUST \
// RUN:   --implicit-check-not="fn c_(" --implicit-check-not="fn C_("

// FR-117 positive pin: a user-defined CONVERSION FUNCTION is OMITTED from
// the class's imported methods, and omitting it keeps the rest of the class
// importable.
//
// What this replaces, measured on unpatched HEAD: `cxxMethodBaseName`
// (lib/ImportC/ImportCFunctions.cpp) fell through to
// `mangleMemberName(method->getName())` for a `DeclarationName` that is not
// an identifier. A `CXXConversionDecl` answers FALSE to
// `isOverloadedOperator()`, so it slipped past the W2.2 member-shape gate in
// `collectRecordFields` entirely, and `getName()` on a
// `CXXConversionFunctionName` returned the EMPTY STRING -- the class emitted
// `fn c_(&self)`, a method with no name anybody can call, and a class with
// TWO conversion functions collided on it outright:
// `error: unsupported: conflicting definition of 'C_'`.
//
// That collision is why this file is a POSITIVE pin and not just a rejection
// pin: the two-conversion-function class below did not import AT ALL in
// strict mode before this change, and now does. It is byte-diffed against
// `clang++ -std=c++17` in test/EndToEnd/cpp-conversion-function.cpp.
//
// Omission (rather than rejecting the whole class) is sound because every
// USE of a conversion function is already a located rejection -- an implicit
// or `static_cast` use is `unsupported cast (UserDefinedConversion)`, and the
// explicit `c.operator int()` spelling is `unsupported: conversion function`
// -- so the omitted method can never be silently called. Both are pinned in
// cpp-conversion-function-invalid.cpp. Note the `getName()` on a
// non-identifier name is itself only latent today: it asserts in
// clang/AST/Decl.h, which is compiled out under our release NDEBUG build.

struct C {
  int v;

  // Ordinary methods of the same class keep their ordinary names: the
  // overload-count loop in `cxxMethodMangledName` walks EVERY method of the
  // class, so an omitted sibling must not perturb them.
  int get() const { return v; }
  void set(int n) { v = n; }

  // Both omitted.
  operator int() const { return v; }
  operator long() const { return v + 1; }
};

int use(int n) {
  C c;
  c.set(n);
  return c.get();
}

// CHECK: emitrust.struct_def @C ["v"] [i32]
// CHECK: func.func @C_get(
// CHECK: func.func @C_set(
// CHECK: func.func @use_(

// RUST: impl C {
// RUST: pub fn c_get(&self) -> i32 {
// RUST: pub fn c_set(&mut self, n: i32) {

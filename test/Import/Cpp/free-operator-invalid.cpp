// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/strict.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT \
// RUN:     --implicit-check-not="INVALID EMPTY SYMBOL" --implicit-check-not="module"
// RUN: not emitrust-import-c %t/twoops.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=TWOOPS \
// RUN:     --implicit-check-not="conflicting definition" \
// RUN:     --implicit-check-not="INVALID EMPTY SYMBOL"
// RUN: not emitrust-import-c %t/litop.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=LITOP \
// RUN:     --implicit-check-not="INVALID EMPTY SYMBOL"
// RUN: emitrust-cc --recover --emit=rust %t/recover.cpp \
// RUN:   | FileCheck %s --check-prefix=REC --implicit-check-not="fn ("
// RUN: emitrust-cc --recover --emit=rust %t/recover.cpp 2>&1 >/dev/null \
// RUN:   | FileCheck %s --check-prefix=RECDIAG
// RUN: emitrust-import-c %t/friend.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FRIEND \
// RUN:     --implicit-check-not="error" --implicit-check-not="INVALID EMPTY SYMBOL"
// RUN: not emitrust-import-c %t/friendbad.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FRIENDBAD \
// RUN:     --implicit-check-not="INVALID EMPTY SYMBOL" --implicit-check-not="module"

// FR-119: a free NON-MEMBER operator is an ordinary top-level FunctionDecl,
// so it slipped past FR-117's `cxxMethod &&`-gated refusal; its
// non-identifier DeclarationName made `cFunctionSymbolName` return "" (the
// getName() assert is compiled out under NDEBUG), and the crate carried the
// literally unparseable `fn (v0: A, b: i32)` with NO diagnostic anywhere --
// a silent invalid-crate channel. This file pins the located rejection that
// closes it: the guard sits AFTER the referenced-only prototype skip in
// `importFunction` (so an unreferenced body-less operator prototype, e.g.
// stl-map-invalid.cpp's free `operator<`, keeps skipping silently BY
// DESIGN), rides the existing `cxx-operator-overload` ledger tag, and
// covers literal operators through the same DeclarationName test.
//
// W2.25 moved the pin FORWARD: free operators of the ADMITTED by-value
// kinds (+, -, *, /, ==, ... — the table in CSymbolNaming.h) now import
// under synthesized identifier spellings (test/Import/Cpp/
// operator-overload.cpp pins the positive half), so every REJECTING shape
// in this file uses a kind OUTSIDE the table (`<<`, `>>`, a literal
// operator) — the guard itself, its recovery drop, and its ledger tag are
// unchanged for everything the wave did not admit.
//
// FR-123 moved the pin forward AGAIN, in the FRIEND sections below: a
// friend function DEFINED INLINE in a class never reached the guard at
// all, because it never reached the item walk at all. It does now, so the
// admitted kinds import (FRIEND) and the non-admitted kinds reach this
// file's own guard and reject located (FRIENDBAD), where both used to be
// silently omitted.

//--- strict.cpp
// The 2-line silent shape: exactly ONE free operator over an admitted
// class used to emit `func.func @<<INVALID EMPTY SYMBOL>>` and exit 0.
// Now: located error at the declaration, no module printed. (`<<` is a
// non-admitted kind — W2.25 flipped the original `operator+` positive.)
// STRICT: strict.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: overloaded operator
struct A {
  int v;
};
int operator<<(A a, int b) { return a.v << b; }

//--- twoops.cpp
// TWO free operators used to collide in the FR-108 cross-TU guard and leak
// `unsupported: conflicting definition of ''` (the empty emitted name).
// The relocated guard fires at the FIRST operator's declaration instead;
// the '' leak is pinned gone by --implicit-check-not above.
// TWOOPS: twoops.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: overloaded operator
struct A {
  int v;
};
int operator<<(A a, int b) { return a.v << b; }
int operator>>(A a, int b) { return a.v >> b; }

//--- litop.cpp
// A literal operator (operator""_kb) is the same non-identifier
// DeclarationName shape and rides the same guard. It used to reject only
// at the CALL (`unsupported callee`); now the error is LOCATED at the
// declaration itself.
// LITOP: litop.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: overloaded operator
unsigned long long operator""_kb(unsigned long long v) { return v * 1024; }
int main(void) { return (int)2_kb; }

//--- recover.cpp
// Under --recover the operator drops as its OWN item (existing
// cxx-operator-overload tag -- no new ledger row) and the recovered crate
// builds: the nameless `fn (` is pinned absent by --implicit-check-not,
// the call site stubs, and everything else still emits.
// REC: struct A
// REC: fn use_
// REC: unimplemented!("unsupported callee")
// REC: fn c_main
// RECDIAG: dropped 'operator<<' [cxx-operator-overload] unsupported: overloaded operator
struct A {
  int v;
};
int operator<<(A a, int b) { return a.v << b; }
int use(A a) { return a << 5; }
int main(void) {
  A a;
  a.v = 1;
  return use(a);
}

//--- friend.cpp
// THE PIN THIS FR MOVED. It used to read: "a friend operator defined
// INLINE in the class is still silently omitted (the class imports, the
// operator leaves no trace, zero diagnostics) ... a SEPARATE pre-existing
// channel -- it never reaches `importFunction` at item scope -- filed as
// its own FR (FR-123); this pin keeps the channel measured as-is until
// that FR moves it."
//
// FR-123 moved it. The channel was never an unsupported CONSTRUCT: the
// same `operator+` written at namespace scope imports today under W2.25's
// admitted by-value table, so the friend spelling was merely NEVER
// REACHED -- its `FunctionDecl` hangs off a `FriendDecl` inside the
// `CXXRecordDecl` and is absent from the TU's own `decls()`. So the fix
// makes it WORK rather than adding a diagnostic: a definition-carrying
// friend is routed through the same arm a top-level `FunctionDecl` takes,
// and this admitted `operator+` now emits `op_add` exactly as the free
// spelling does. The positive half (byte-identical friend-vs-free
// emission, overload suffixes, the no-double-import prototype rule) is
// pinned in test/Import/Cpp/friend-inline-definition.cpp; the ledger half
// -- the operator is now IN the `--incremental` denominator, where a
// missing function used to read 1000 permille -- in
// test/Driver/incremental-friend-operator.cpp.
// FRIEND: emitrust.struct_def @B
// FRIEND: func.func @op_add(
// FRIEND: func.func @c_main
struct B {
  int v;
  friend int operator+(B a, int b) { return a.v + b; }
};
int main(void) {
  B b;
  b.v = 2;
  return b.v;
}

//--- friendbad.cpp
// The other half of the same move, and the one that keeps this file's
// charter: a friend operator whose kind is OUTSIDE the admitted table now
// reaches THIS FILE'S guard instead of being dropped on the floor. Before
// FR-123 this program exited 0 with a module containing `B` and `c_main`,
// no operator and NO DIAGNOSTIC -- a silent loss the ledger scored as
// fully ported. Now it is the same located `unsupported: overloaded
// operator` a free `operator<<` earns, on the same `cxx-operator-overload`
// ledger tag, and no module is printed.
// FRIENDBAD: friendbad.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: overloaded operator
struct B {
  int v;
  friend int operator<<(B a, int b) { return a.v << b; }
};
int main(void) {
  B b;
  b.v = 2;
  return b.v;
}

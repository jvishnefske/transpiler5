// RUN: split-file %s %t
// RUN: emitrust-cc --recover --emit=rust %t/class-gate.cpp \
// RUN:   | FileCheck %s --check-prefix=GATE \
// RUN:     --implicit-check-not="struct C" --implicit-check-not="impl Drop" \
// RUN:     --implicit-check-not="c_dtor" --implicit-check-not="c_get"
// RUN: emitrust-cc --recover --emit=rust %t/class-gate.cpp 2>&1 >/dev/null \
// RUN:   | FileCheck %s --check-prefix=GATEDIAG
// RUN: emitrust-cc --recover --emit=mlir %t/class-gate.cpp 2>/dev/null \
// RUN:   | FileCheck %s --check-prefix=GATEIR --implicit-check-not="struct_def"
// RUN: emitrust-cc --recover --emit=rust %t/body-fail.cpp \
// RUN:   | FileCheck %s --check-prefix=BODY \
// RUN:     --implicit-check-not="struct C" --implicit-check-not="impl Drop"
// RUN: emitrust-cc --recover --emit=rust %t/body-fail.cpp 2>&1 >/dev/null \
// RUN:   | FileCheck %s --check-prefix=BODYDIAG
// RUN: emitrust-cc --recover --emit=rust %t/name-claim.cpp \
// RUN:   | FileCheck %s --check-prefix=CLAIM \
// RUN:     --implicit-check-not="collides with the emitted name"

// FR-118: a class that fails a class-level gate must contribute NOTHING to
// the emitted crate. This file pins the invariant W2.2's comments CLAIMED
// ("a rejected class never half-imports") but that the code did not hold:
// `importCXXMethods` runs AFTER the `StructDefOp` is created and after
// `structSymbolName` has claimed the emitted name, and on HEAD neither was
// undone when a method failed. Measured on unpatched HEAD, all three
// sections below emitted `struct C { v: i32 }` -- plus, for class-gate, an
// `impl Drop for C` and an `impl C` built from the OUT-OF-LINE definitions,
// which reach `importFunction` directly and are not gated by
// `rejectedRecords` -- while every user of the class cascaded.
//
// That is not merely dead weight. The cross-translation-unit consequence is
// a SILENT MISCOMPILE, byte-diffed in
// test/EndToEnd/cpp-rejected-class-no-trace.cpp: a plain POD in another TU
// whose emitted name and field shape coincide with the rejected class MERGES
// with the leftover struct_def and inherits its `impl Drop`, so the crate
// runs a destructor the C++ program never runs. This file is the
// single-TU half of that regression.
//
// `body-fail` is the section that decides the SHAPE of the fix. design.md's
// FR-118 proposed hoisting the copy/move/delegating-constructor check into
// `collectRecordFields` "and auditing `importCXXMethods` so nothing left in
// it can fail the record". That audit is not achievable: pass 2 imports an
// ARBITRARY method body and can fail on any unsupported construct in the
// language, which `body-fail` exercises and which reproduces the identical
// half-import. The fix that ships is therefore the UNDO on the failure path
// (erase the struct_def, the class's method funcs, and every name-registry
// entry the record claimed), not the hoist.
//
// `name-claim` is the bonus the undo buys: on HEAD a rejected class kept its
// claim on the emitted name and STARVED a perfectly importable sibling,
// which was dropped with `[record-name-clash]` -- two items lost, the dead
// one kept and the live one discarded. Recovery is `--recover` throughout:
// plain `--emit=rust` is strict and emits nothing at all.

//--- class-gate.cpp
extern "C" int printf(const char *, ...);

// Rejected at the class level (the copy constructor), i.e. from inside
// `importCXXMethods`, AFTER the struct_def already exists.
struct C {
  int v;
  C() : v(1) {}
  C(const C &o) : v(2) {}
  int get() const;
  ~C();
};

// Both out-of-line definitions are TOP-LEVEL items: they reach
// `importFunction` on their own and were the third leg of the half-import.
// With the class's name registration undone they take the located
// `method of an unimported class` cascade instead.
int C::get() const { return v; }
C::~C() { printf("dtor %d\n", v); }

int uses(int n) {
  C c;
  c.v = n;
  return c.get();
}

int survivor(int n) { return n + 1; }

// GATE: fn survivor
// GATEIR: emitrust.func @survivor
// GATEDIAG: class-gate.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: copy/move/delegating constructor (recovered: item dropped)
// GATEDIAG: class-gate.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: method of an unimported class (recovered: item dropped)
// GATEDIAG: class-gate.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: method of an unimported class (recovered: item dropped)
// GATEDIAG: class-gate.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: struct 'C' was rejected, so a type naming it cannot be imported (recovered: emitted an unimplemented!() stub with the mapped signature)

//--- body-fail.cpp
// The failure is in the BODY of an ordinary method -- no class-level
// predicate can hoist it -- and it must still leave no trace.
struct C {
  int v;
  C() : v(1) {}
  int bad() const { return *(int *)(long)v; }
};

int uses(int n) {
  C c;
  c.v = n;
  return c.bad();
}

int survivor(int n) { return n + 2; }

// BODY: fn survivor
// BODYDIAG: body-fail.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported pointer expression: CStyleCastExpr (recovered: item dropped)
// BODYDIAG: body-fail.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: struct 'C' was rejected, so a type naming it cannot be imported (recovered: emitted an unimplemented!() stub with the mapped signature)

//--- name-claim.cpp
// `C` is rejected; `struct c` idiomatically renames to the SAME emitted
// name. On HEAD the rejected class kept the claim and this importable POD
// was dropped as a name clash.
struct C {
  int v;
  C() : v(1) {}
  C(const C &o) : v(2) {}
};

struct c {
  int w;
};

int good(int n) {
  struct c q;
  q.w = n;
  return q.w;
}

// CLAIM:      struct C {
// CLAIM-NEXT:     w: i32,
// CLAIM-NEXT: }
// CLAIM:      fn good

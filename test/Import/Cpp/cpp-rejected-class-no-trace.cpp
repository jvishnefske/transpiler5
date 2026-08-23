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
// RUN:     --implicit-check-not="impl Drop" --implicit-check-not="fn c_bad"
// RUN: emitrust-cc --recover --emit=rust %t/body-fail.cpp 2>&1 >/dev/null \
// RUN:   | FileCheck %s --check-prefix=BODYDIAG
// RUN: emitrust-cc --recover --emit=rust %t/dtor-body-fail.cpp \
// RUN:   | FileCheck %s --check-prefix=DTORFAIL \
// RUN:     --implicit-check-not="struct C" --implicit-check-not="impl Drop" \
// RUN:     --implicit-check-not="c_dtor"
// RUN: emitrust-cc --recover --emit=rust %t/dtor-body-fail.cpp 2>&1 >/dev/null \
// RUN:   | FileCheck %s --check-prefix=DTORFAILDIAG
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
// `body-fail` decided the SHAPE of the FR-118 fix (the undo, not the
// hoist: pass 2 imports an ARBITRARY method body and can fail on any
// unsupported construct, so no class-level predicate exists to move
// earlier) -- and was then RE-DECIDED by FR-112 containment: an ORDINARY
// method whose body fails no longer rejects the class at all. The method
// is OMITTED (the warning carries its own per-construct diagnostic), the
// class and its importable ctor emit normally, and the USE of the omitted
// method is what cascades -- `uses` is stubbed on the located `call to
// unimported method`, not on a whole-class `struct 'C' was rejected`. The
// no-trace invariant this file pins therefore narrows to what it always
// really protected: the CLASS-LEVEL failure paths.
//
// `dtor-body-fail` is the section that keeps the FR-118 undo honest now
// that ordinary bodies are contained: a DESTRUCTOR body failure cannot be
// contained (a destructor is invoked implicitly at scope exit -- there is
// no call node to reject, exactly the copy-ctor argument), so it is the
// surviving in-`importCXXMethods` body failure that must still erase the
// struct_def, the method funcs, and every name-registry claim.
//
// `name-claim` is the bonus the undo buys: on HEAD a rejected class kept its
// claim on the emitted name and STARVED a perfectly importable sibling,
// which was dropped with `[record-name-clash]` -- two items lost, the dead
// one kept and the live one discarded. Recovery is `--recover` throughout:
// plain `--emit=rust` is strict and emits nothing at all.

//--- class-gate.cpp
extern "C" int printf(const char *, ...);

// Rejected at the class level (the MOVE constructor -- the copy ctor
// specimen this section used before W2.23 is admitted now, and a move
// ctor is the same in-`importCXXMethods`, after-the-struct_def gate),
// i.e. from inside `importCXXMethods`, AFTER the struct_def already
// exists.
struct C {
  int v;
  C() : v(1) {}
  C(C &&o) : v(2) {}
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
// FR-112: the failure is in the BODY of an ORDINARY method, so the method
// is omitted and the CLASS stays. The struct, its default constructor and
// the untouched sibling method all emit; only the USER of the omitted
// method is stubbed, on the located call-site rejection.
struct C {
  int v;
  C() : v(1) {}
  int bad() const { return *(int *)(long)v; }
  int fine() const { return v + 1; }
};

int uses(int n) {
  C c;
  c.v = n;
  return c.bad();
}

int survivor(int n) { return n + 2; }

// BODY: struct C {
// BODY: fn survivor
// BODY: impl C {
// BODY: fn c_new
// BODY: fn c_fine
// BODYDIAG: body-fail.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported pointer expression: CStyleCastExpr (omitted: method 'bad' of class 'C')
// BODYDIAG: body-fail.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: call to unimported method 'c_bad' (recovered: emitted an unimplemented!() stub with the mapped signature)

//--- dtor-body-fail.cpp
// The surviving class-level BODY failure: a destructor is invoked
// implicitly, so its failing body cannot be contained and the FR-118 undo
// must still leave no trace.
extern "C" int printf(const char *, ...);

struct C {
  int v;
  C() : v(1) {}
  ~C() { printf("dtor %d\n", *(int *)(long)v); }
};

int uses(int n) {
  C c;
  c.v = n;
  return c.v;
}

int survivor(int n) { return n + 4; }

// DTORFAIL: fn survivor
// DTORFAILDIAG: dtor-body-fail.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported pointer expression: CStyleCastExpr (recovered: item dropped)
// DTORFAILDIAG: dtor-body-fail.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: struct 'C' was rejected, so a type naming it cannot be imported (recovered: emitted an unimplemented!() stub with the mapped signature)

//--- name-claim.cpp
// `C` is rejected (move ctor -- the W2.23-admitted copy ctor no longer
// serves as the specimen); `struct c` idiomatically renames to the SAME
// emitted name. On HEAD the rejected class kept the claim and this
// importable POD was dropped as a name clash.
struct C {
  int v;
  C() : v(1) {}
  C(C &&o) : v(2) {}
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

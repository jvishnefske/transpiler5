// RUN: split-file %s %t
// RUN: emitrust-import-c %t/instance.cpp | FileCheck %s --check-prefix=INST \
// RUN:   --implicit-check-not="Chain_lead" --implicit-check-not="Chain_bad"
// RUN: emitrust-import-c %t/instance.cpp 2>&1 >/dev/null \
// RUN:   | FileCheck %s --check-prefix=INSTDIAG \
// RUN:     --implicit-check-not="does not reference a valid function" \
// RUN:     --implicit-check-not="empty block"
// RUN: emitrust-cc --emit=rust %t/instance.cpp | FileCheck %s --check-prefix=RUST \
// RUN:   --implicit-check-not="chain_lead" --implicit-check-not="chain_bad"
// RUN: emitrust-import-c %t/static.cpp | FileCheck %s --check-prefix=STATIC \
// RUN:   --implicit-check-not="SChain_lead" --implicit-check-not="SChain_helper"
// RUN: emitrust-import-c %t/static.cpp 2>&1 >/dev/null \
// RUN:   | FileCheck %s --check-prefix=STATICDIAG \
// RUN:     --implicit-check-not="does not reference a valid function" \
// RUN:     --implicit-check-not="empty block"

// FR-112 constraint C2, the FIXPOINT: the per-class method import must
// re-run both FR-47 passes from a clean slate of the class's funcs until
// the omitted set stops growing. Both sections declare the CALLER BEFORE
// the failing CALLEE on purpose -- the FR-47 signature prepass registers a
// stub for the callee, the caller imports a call against it, and THEN the
// callee's body fails. A single-pass omission would erase the callee's func
// and leave the caller holding a dangling reference that kills the whole
// TU with a diagnostic attributable to no item:
//
//  - `instance` is the loud variant: a dangling `func.call` fails the
//    verifier with `'func.call' op 'Chain_bad' does not reference a valid
//    function` (measured by hand-erasing the callee at the spike). Loud,
//    but it is a TU-level error under recovery, not a rejectable item --
//    the FR-118 spike measured this exact shape emitting `error: empty
//    block: expect at least a terminator` and NO crate at all. Both
//    strings are implicit-check-nots here.
//  - `static` is the SILENT variant, FR-112's constraint C8: a resolved
//    static call renders as an opaque callee STRING (`emitrust.call_opaque`),
//    which the verifier never checks, so a missed fixpoint re-run would
//    sail through `emitrust-opt` and surface only as rustc E0599 at cargo
//    time. This import-level pin is the only tier that catches it.
//
// With the fixpoint in place the cascade is finite and each round is
// attributed: the callee is omitted with its own per-construct diagnostic,
// the re-run then rejects the caller AT ITS CALL (`call to unimported
// method`), the caller joins the omitted set, and the class imports with
// the untouched sibling intact. The omitted set only grows and is bounded
// by the method count, so termination is structural.

//--- instance.cpp
struct Chain {
  int v;
  // Caller first: imports cleanly against the callee's pass-1 stub.
  int lead(int n) { return bad(n) + 1; }
  // Callee fails in pass 2 (unsupported pointer cast).
  int bad(int n) { return *(int *)(long)n; }
  // Untouched sibling: must survive every fixpoint round.
  int good(int n) { return v + n + 3; }
};
int use(int n) {
  Chain c;
  c.v = n;
  return c.good(n);
}

// INST: emitrust.struct_def @Chain ["v"] [i32]
// INST: func.func @Chain_good(
// INST: func.func @use_(

// INSTDIAG: instance.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported pointer expression: CStyleCastExpr (omitted: method 'bad' of class 'Chain')
// INSTDIAG: instance.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: call to unimported method 'Chain_bad' (omitted: method 'lead' of class 'Chain')

// RUST: impl Chain {
// RUST: pub fn chain_good(&mut self, n: i32) -> i32 {

//--- static.cpp
struct SChain {
  int v;
  int lead(int n) { return helper(n) + 1; }
  static int helper(int n) { return *(int *)(long)n; }
  int good(int n) { return v + n + 4; }
};
int use(int n) {
  SChain s;
  s.v = n;
  return s.good(n);
}

// STATIC: emitrust.struct_def @SChain ["v"] [i32]
// STATIC: func.func @SChain_good(
// STATIC: func.func @use_(

// STATICDIAG: static.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported pointer expression: CStyleCastExpr (omitted: method 'helper' of class 'SChain')
// STATICDIAG: static.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: call to unimported method 'SChain_helper' (omitted: method 'lead' of class 'SChain')

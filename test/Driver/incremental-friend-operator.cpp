// RUN: split-file %s %t.dir
// RUN: rm -rf %t.crate
// RUN: emitrust-cc --emit=crate --incremental %t.dir/good.cpp -o %t.crate \
// RUN:   --crate-type=lib
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/lib.rs
//
// RUN: rm -rf %t.bad.crate
// RUN: emitrust-cc --emit=crate --incremental %t.dir/bad.cpp -o %t.bad.crate \
// RUN:   --crate-type=lib 2>%t.bad.err
// RUN: FileCheck %s --check-prefix=BADDIAG --input-file=%t.bad.err
// RUN: FileCheck %s --check-prefix=BADJSON \
// RUN:   --input-file=%t.bad.crate/emitrust-progress.json

// FR-123, the LEDGER half: an item the importer cannot see must never be
// counted as ported.
//
// The defect this pins closed is not "a function went missing" -- it is
// "the artifact REPORTED SUCCESS for incomplete output". A friend operator
// defined inline in its class hangs off a `FriendDecl` inside the
// `CXXRecordDecl` and is absent from the translation unit's `decls()`, so
// neither the importer's item walk nor the FR-40 item graph (which mirrors
// that walk decl-for-decl, by design and by necessity) ever reached it.
// The operator was therefore not even in the DENOMINATOR, and
// `--incremental` wrote, for a program that had silently lost a function:
//
//     "graph_items": 2, "ported": 2, "missing": 0, "ported_permille": 1000
//
// That is FR-143's artifact-untruth class in its most dangerous direction:
// 1000 permille claimed for output that is not the program. The fix makes
// the operator a real item in every walk that must agree on the item set,
// so it is now in the denominator AND in the numerator, honestly.
//
// The other direction of the same truth is the `bad.cpp` half: a friend
// operator OUTSIDE the W2.25 admitted by-value table is now a LOCATED
// rejection rather than a silent omission. It composes no emitted symbol,
// so the graph mints no node for it (exactly as for a free operator of the
// same kind) and it is tallied in `off_graph_rejected` with its own
// location and blocker tag, instead of vanishing without trace.

//--- good.cpp
// The operator is DEFINED but never used, which is what made the pre-fix
// loss silent: with a use, the call site rejected loudly ("call to
// unimported function 'op_add'"); with none, nothing complained at all.
// JSON: "graph_items": 3,
// JSON-NEXT: "ported": 3,
// JSON-NEXT: "stubbed": 0,
// JSON-NEXT: "dropped": 0,
// JSON-NEXT: "missing": 0,
// JSON: "off_graph_rejected": 0,
// JSON-NEXT: "ported_permille": 1000
// JSON: "symbol": "V",
// JSON: "symbol": "op_add",
// JSON-NEXT: "kind": "function",
// JSON-NEXT: "status": "ported",
// JSON: "symbol": "use_",
//
// RUST: fn op_add(a: &V, b: &V) -> V
struct V {
  int x;
  friend V operator+(const V &a, const V &b) { V r; r.x = a.x + b.x; return r; }
};
int use(int n) {
  V a;
  a.x = n;
  return a.x;
}

//--- bad.cpp
// `operator<<` is not in the W2.25 admitted by-value table, so the friend
// walk hands it to FR-119's guard and it drops as its own item on the
// existing `cxx-operator-overload` ledger tag. Pre-fix: exit 0, no
// diagnostic, no item, no trace anywhere.
// BADDIAG: bad.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: overloaded operator
//
// BADJSON: "off_graph_rejected": 1,
// BADJSON: "off_graph_items": [
// BADJSON: "blocker": "cxx-operator-overload"
struct B {
  int v;
  friend int operator<<(const B &a, int b) { return a.v << b; }
};
int use(int n) {
  B a;
  a.v = n;
  return a.v;
}

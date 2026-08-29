// RUN: split-file %s %t
//
// The admitted friend operator imports, under the SAME symbol as the free
// spelling, and its use resolves.
// RUN: emitrust-import-c %t/op.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=OP \
// RUN:     --implicit-check-not="error" --implicit-check-not="INVALID EMPTY SYMBOL"
// The acceptance criterion, stated as a byte-diff rather than as a pattern:
// the friend spelling and the free spelling emit the IDENTICAL crate.
// RUN: emitrust-cc --emit=rust %t/op.cpp > %t/op.rs
// RUN: emitrust-cc --emit=rust %t/opfree.cpp > %t/opfree.rs
// RUN: diff %t/op.rs %t/opfree.rs
//
// A NAMED (non-operator) friend function rides the same channel and is
// admitted with it, again byte-identical to its free spelling.
// RUN: emitrust-cc --emit=rust %t/named.cpp > %t/named.rs
// RUN: emitrust-cc --emit=rust %t/namedfree.cpp > %t/namedfree.rs
// RUN: diff %t/named.rs %t/namedfree.rs
//
// A genuine friend OVERLOAD SET splits on FR-114's per-parameter suffixes,
// exactly as the free spelling does, and each call resolves to its own.
// RUN: emitrust-import-c %t/overload.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=OVERLOAD --implicit-check-not="error"
//
// A body-less friend PROTOTYPE plus an out-of-class definition must import
// the operator exactly ONCE (the out-of-class definition is already a
// top-level declaration; the prototype must not double-import it).
// RUN: emitrust-import-c %t/proto.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=PROTO \
// RUN:     --implicit-check-not="error" \
// RUN:     --implicit-check-not="conflicting definition"
//
// A friend CLASS declaration declares no code and stays a no-op.
// RUN: emitrust-import-c %t/fclass.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FCLASS --implicit-check-not="error"
//
// A hidden friend defined in a HEADER two translation units include is the
// pre-existing FR-58 vague-linkage gap, reached now that the definition is
// visible. It must land on the SAME located rejection an `inline` free
// function in the same header earns -- never a silent merge of two
// definitions, and never the pre-FR-123 call-site error blaming the caller.
// RUN: not emitrust-cc --emit=rust %t/mtu-a.cpp %t/mtu-b.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=MTU
// RUN: not emitrust-cc --emit=rust %t/mtu-c.cpp %t/mtu-d.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=MTUFREE

// FR-123. A friend function DEFINED INLINE in a class was SILENTLY
// OMITTED: its `FunctionDecl` hangs off a `FriendDecl` inside the
// `CXXRecordDecl` and is absent from the translation unit's own
// `decls()`, so `importDeclsIn`'s item walk never visited it, no
// diagnostic was raised anywhere, and the emitted crate simply did not
// contain the function. Worse than invisible: because the item graph
// mirrors that same walk, `--incremental` reported
// `graph_items: 2, ported: 2, ported_permille: 1000` for a program that
// had lost a function — 100% ported claimed for incomplete output (the
// FR-143 artifact-untruth class, in its most dangerous direction). Pinned
// as-is by free-operator-invalid.cpp's FRIEND section until this FR; that
// pin has now moved forward, and test/Driver/incremental-friend-operator.cpp
// pins the ledger half.
//
// The FR's framing was rejection-with-a-diagnostic; MEASUREMENT said the
// construct is ALREADY FULLY SUPPORTED — the identical operator written at
// NAMESPACE scope imports today as `fn op_add(a: &V, b: &V) -> V` with zero
// diagnostics (W2.25's admitted by-value free-operator table in
// CSymbolNaming.h). The friend spelling was not an unsupported construct,
// it was simply never REACHED. So the fix MAKES IT WORK: a
// definition-carrying friend is routed through the very same arm a
// top-level `FunctionDecl` takes, in all four walks that must agree on the
// item set (`CImporter::importDeclsIn`,
// `CImporter::collectOrdinaryNamesFrom`, `ItemGraphBuilder`'s three passes,
// `AdmissibilityProbe::probeDeclsIn`). A friend shape OUTSIDE the admitted
// table therefore gets FR-119's LOCATED rejection instead of silence —
// pinned in free-operator-invalid.cpp's FRIENDBAD section.
//
// The selection rule, and why each clause is load-bearing:
//  * DEFINITION-CARRYING ONLY. A body-less friend prototype whose
//    definition is written out of class is skipped here, because that
//    definition is already a top-level item; importing at both sites would
//    be a double definition (the `proto.cpp` section).
//  * NON-TEMPLATE ONLY. A friend function TEMPLATE, and a friend of a
//    CLASS template, are not visited: a class template's item arm walks
//    `specializations()`, and one hidden-friend symbol per instantiation
//    has no suffix distinguishing the instantiations (`Box<int>` and
//    `Box<long>` would both claim `op_add`). Recorded, unchanged: a use
//    still fails loudly at the call site with "call to unimported
//    function", never a silent wrong symbol.
//  * NON-MEMBER ONLY. `friend void Other::f();` names another class's
//    METHOD; it carries no body at the friend site and is skipped by the
//    definition clause, and members are additionally screened out.
//  * TOP-LEVEL RECORDS ONLY. A friend of a NESTED class is not reached,
//    because the nested class itself is already a loud rejection
//    ("unsupported: struct definition outside file or function scope"), so
//    nothing is lost silently behind it.

//--- op.cpp
// The hidden-friend spelling. Before FR-123: `emitrust-cc --emit=rust`
// exited 0 emitting `struct V` and `c_main` with NO `op_add` and NO
// diagnostic, and a USE rejected with "call to unimported function
// 'op_add'".
// OP: emitrust.struct_def @V
// OP: func.func @op_add(
// OP-SAME: !emitrust.ref<!emitrust.struct<"V">>, %{{.*}}: !emitrust.ref<!emitrust.struct<"V">>) -> !emitrust.struct<"V">
// OP: func.func @use_
// OP: call @op_add(
struct V {
  int x;
  friend V operator+(const V &a, const V &b) { V r; r.x = a.x + b.x; return r; }
};
int use(int n) {
  V a;
  a.x = n;
  V b;
  b.x = n + 1;
  V c = a + b;
  return c.x;
}

//--- opfree.cpp
// The free spelling of the very same program: byte-identical emission is
// the acceptance criterion, so this file must stay a character-for-
// character copy of op.cpp with the operator moved out of the class.
struct V {
  int x;
};
V operator+(const V &a, const V &b) { V r; r.x = a.x + b.x; return r; }
int use(int n) {
  V a;
  a.x = n;
  V b;
  b.x = n + 1;
  V c = a + b;
  return c.x;
}

//--- named.cpp
// A NAMED friend function was lost through the identical channel (measured
// pre-fix: "call to unimported function 'total'"), so it is admitted with
// the operators rather than left as a second silent hole.
struct V {
  int x;
  friend int total(const V &a) { return a.x + 1; }
};
int use(int n) {
  V a;
  a.x = n;
  return total(a);
}

//--- namedfree.cpp
struct V {
  int x;
};
int total(const V &a) { return a.x + 1; }
int use(int n) {
  V a;
  a.x = n;
  return total(a);
}

//--- overload.cpp
// Two hidden friends sharing one synthesized base: the FR-114 suffixes
// split them, and they are computed from `overloadSetSize`'s
// `DeclContext::lookup`, which DOES see a hidden friend — which is why
// the definition site and the call site agree by construction (pre-fix the
// call site already composed `op_add_rv_rv` while nothing had been
// emitted under that name).
// OVERLOAD: func.func @op_add_rv_rv(
// OVERLOAD: func.func @op_add_rv_i32(
// OVERLOAD: func.func @use_
// OVERLOAD: call @op_add_rv_rv(
// OVERLOAD: call @op_add_rv_i32(
struct V {
  int x;
  friend V operator+(const V &a, const V &b) { V r; r.x = a.x + b.x; return r; }
  friend V operator+(const V &a, int b) { V r; r.x = a.x + b; return r; }
};
int use(int n) {
  V a;
  a.x = n;
  V b;
  b.x = n + 1;
  V c = a + b;
  V d = a + n;
  return c.x + d.x;
}

//--- proto.cpp
// The no-double-import clause. The friend declaration has no body, so the
// friend walk skips it entirely and the out-of-class definition imports
// once, exactly as it did before FR-123.
// PROTO: emitrust.struct_def @V
// PROTO: func.func @op_add(
// PROTO: func.func @use_
// PROTO-NOT: func.func @op_add(
struct V {
  int x;
  friend V operator+(const V &a, const V &b);
};
V operator+(const V &a, const V &b) { V r; r.x = a.x + b.x; return r; }
int use(int n) {
  V a;
  a.x = n;
  V b;
  b.x = n + 1;
  V c = a + b;
  return c.x;
}

//--- fclass.cpp
// A friend TYPE declaration grants access and declares no code; it has no
// `FunctionDecl` at all, so the walk contributes nothing for it. Pinned so
// the friend walk cannot start minting items for access grants.
// FCLASS: emitrust.struct_def @W
// FCLASS: emitrust.struct_def @V
// FCLASS: func.func @use_
// FCLASS-NOT: func.func
struct W;
struct V {
  int x;
  friend class W;
};
struct W {
  int y;
};
int use(int n) {
  V a;
  a.x = n;
  W w;
  w.y = n + 1;
  return a.x + w.y;
}

//--- mtu.h
// MTU: mtu.h:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of 'op_add' (already defined in another translation unit)
struct V {
  int x;
  friend V operator+(const V &a, const V &b) { V r; r.x = a.x + b.x; return r; }
};

//--- mtu-a.cpp
#include "mtu.h"
int adda(int n) { V a; a.x = n; V b; b.x = n + 1; V c = a + b; return c.x; }

//--- mtu-b.cpp
#include "mtu.h"
int adda(int n);
int main(void) { return adda(1); }

//--- mtufree.h
// The control: an ordinary `inline` FREE function in the same position
// earns the identical wording at the identical kind of location, which is
// what makes the hidden-friend result the pre-existing gap rather than a
// new one FR-123 introduced. (Pre-FR-123 the friend half instead said
// "call to unimported function 'op_add'", blaming the CALLER for a
// definition nobody had emitted.)
// MTUFREE: mtufree.h:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of 'plus_w' (already defined in another translation unit)
struct W {
  int x;
};
inline W plus_w(const W &a, const W &b) { W r; r.x = a.x + b.x; return r; }

//--- mtu-c.cpp
#include "mtufree.h"
int addc(int n) { W a; a.x = n; W b; b.x = n + 1; W c = plus_w(a, b); return c.x; }

//--- mtu-d.cpp
#include "mtufree.h"
int addc(int n);
int main(void) { return addc(1); }

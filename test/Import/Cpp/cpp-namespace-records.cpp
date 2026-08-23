// FR-108 (namespace half), the emitted-name pin. Record names take the
// same `ns_<name>_`-per-level prefix `cFunctionSymbolName` has always
// applied (`namespacePrefix`), so two records sharing a C++ spelling in
// different namespaces emit two distinct Rust types with two distinct
// method sets instead of silently merging into the first one.
//
// Pinned here because these are EMITTED SYMBOL NAMES, a byte-identity
// invariant shared with the FR-40 item graph (CSymbolNaming.h): the
// prefix composes outermost-to-innermost for nested namespaces, an
// anonymous namespace contributes the fixed `ns_anon_` tag, and a
// `LinkageSpecDecl` (`extern "C" { ... }`) is transparent and adds
// nothing. Both rename modes are pinned, because the fold order is
// load-bearing: the prefix goes on BEFORE the idiomatic camel fold, so
// `ns::Box` is `NsNsBox` (lint-clean) and never `ns_ns_Box` under a
// rename that would trip rustc's denied non_camel_case_types.
// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=NSBODY
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RENAME

extern "C" int printf(const char *, ...);

struct Box {
  int v;
  int get() const { return v; }
};

namespace ns {
struct Box {
  int v;
  int get() const { return v + 100; }
};
} // namespace ns

namespace a {
namespace b {
struct Pt {
  int x;
  int get() const { return x + 5; }
};
} // namespace b
} // namespace a

namespace {
struct Tally {
  int n;
  int get() const { return n * 3; }
};
} // namespace

namespace cfgns {
extern "C" {
struct Cfg {
  int v;
};
}
} // namespace cfgns

int main(int argc, char **argv) {
  Box g;
  g.v = argc;
  ns::Box n;
  n.v = argc;
  a::b::Pt p;
  p.x = argc;
  Tally t;
  t.n = argc;
  cfgns::Cfg c;
  c.v = argc;
  printf("%d %d %d %d %d\n", g.get(), n.get(), p.get(), t.get(), c.v);
  return 0;
}

// Rename OFF (`emitrust-import-c`): the prefix is the whole change, the
// C spelling of the tag is untouched, and the per-class method mangle
// reads the assigned struct name, so it inherits the prefix for free.
// CHECK-DAG: emitrust.struct_def @Box ["v"] [i32]
// CHECK-DAG: emitrust.struct_def @ns_ns_Box ["v"] [i32]
// CHECK-DAG: emitrust.struct_def @ns_a_ns_b_Pt ["x"] [i32]
// CHECK-DAG: emitrust.struct_def @ns_anon_Tally ["n"] [i32]
// CHECK-DAG: emitrust.struct_def @ns_cfgns_Cfg ["v"] [i32]
// CHECK-DAG: func.func @Box_get
// CHECK-DAG: func.func @ns_ns_Box_get
// CHECK-DAG: func.func @ns_a_ns_b_Pt_get
// CHECK-DAG: func.func @ns_anon_Tally_get
// Two records, two BODIES, in declaration order: `::Box::get` returns the
// field and `ns::Box::get` adds 100. Before FR-108 only the first of
// these existed and BOTH call sites resolved to it.
// NSBODY-LABEL: func.func @Box_get
// NSBODY-NOT: arith.constant 100
// NSBODY-LABEL: func.func @ns_ns_Box_get
// NSBODY: arith.constant 100 : i32

// Rename ON (`emitrust-cc`, FR-53): prefix first, camel fold second.
// RENAME-DAG: struct Box {
// RENAME-DAG: struct NsNsBox {
// RENAME-DAG: struct NsANsBPt {
// RENAME-DAG: struct NsAnonTally {
// RENAME-DAG: struct NsCfgnsCfg {
// FR-110: the namespace flattening lives in the STRUCT/impl names; the
// members themselves print the bare in-impl spelling `get` inside each
// class's own (still-distinct) impl. The four mangled module symbols
// (`ns_ns_box_get`, ...) remain the IR/symbol-surface names above.
// RENAME: impl Box {
// RENAME-NEXT: fn get(
// RENAME: impl NsNsBox {
// RENAME-NEXT: fn get(
// RENAME: impl NsANsBPt {
// RENAME-NEXT: fn get(
// RENAME: impl NsAnonTally {
// RENAME-NEXT: fn get(

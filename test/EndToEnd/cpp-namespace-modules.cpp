// REQUIRES: cargo
// FR-231 `--namespace-modules`, the RUNTIME oracle.
//
// `cargo build` succeeding is compile-only evidence and cannot see a
// miscompile, so a flag that rewrites every namespaced symbol from
// `ns_geo_ns_inner_thrice` into the path `crate::geo::inner::thrice` is not
// proved by an emitted crate that builds. This file diffs the built crate's
// stdout against the clang++-built native, BOTH ways, over one source:
//
//   * the flag ON leg -- the feature actually behaves;
//   * the flag OFF leg -- the SYMMETRY pin. The same program, transpiled with
//     the flag absent, must produce the same bytes. A naming change that
//     altered observable behavior in either direction would show here and
//     nowhere in the golden tests.
//
// Every value derives from argc, so constant folding cannot pre-compute the
// answers and hide a miscompile behind a compile-clean crate.
//
// The shapes chosen are the ones each of FR-231's four verified gaps lives in:
//   * NESTED namespaces (`geo::inner`, and a three-deep `outer::mid::deep`),
//     which the pre-FR-231 renderer would have written as the unparsable
//     `mod geo::inner {`;
//   * a namespaced record WITH METHODS (`geo::Vec`), whose `impl` has no
//     module form and stays at the crate root as `impl crate::geo::Vec`, plus
//     a namespaced record with a DESTRUCTOR (`geo::Trace`), whose `Drop` impl
//     takes the same treatment -- the printf in the destructor makes drop
//     presence and drop TIMING observable, which a byte-diff over a
//     destructor-free program cannot see;
//   * a namespaced mutable GLOBAL (`geo::calls`) read and written by three
//     namespaced functions. Its FR-62 owner lift pulls those functions out of
//     the module and onto a synthesized owner struct, which is exactly the
//     path where the method CALL site had to learn to spell the leaf: passing
//     the symbol through wrote `self.crate::geo::twice(..)`, an exit-0
//     unbuildable crate. `calls` is printed, so the lift's arithmetic is
//     observable and not merely compiled;
//   * namespace REOPENING (`geo::quad` comes from a second `namespace geo`
//     block) and an ANONYMOUS namespace (`hidden`);
//   * a namespace-free function (`flat`) beside all of it, unaffected;
//   * a module that NAMES A CRATE-ROOT ITEM (`RootPair`, `root_add`). FR-173's
//     `use super::*;` covers that at depth 1, but at depth > 1 `super` is the
//     enclosing NAMESPACE module, not the crate root, and a glob import is not
//     re-exported through the parent's own glob -- so a nested module takes
//     `use crate::*;` instead. `deep::five` is three levels down and reaches
//     both root items; `geo::pure` reaches them from depth 1. Pinned as a
//     spelling by the MOD leg and as behavior by the diff.
//
// RUN: emitrust-cc --emit=rust --namespace-modules %s | FileCheck %s --check-prefix=MOD
// RUN: emitrust-cc --emit=crate --namespace-modules %s -o %t.mod \
// RUN:   --crate-name nsmod_on --build
// RUN: emitrust-cc --emit=crate %s -o %t.flat --crate-name nsmod_off --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.mod/target/release/nsmod_on > %t.mod.out
// RUN: %t.flat/target/release/nsmod_off > %t.flat.out
// RUN: diff %t.native.out %t.mod.out
// RUN: diff %t.native.out %t.flat.out

extern "C" int printf(const char *, ...);

struct RootPair {
  int a;
  int b;
};

int root_add(int x, int y) { return x + y; }

namespace geo {
int calls = 0;
struct Vec {
  int x;
  int y;
  int len2() const { return x * x + y * y; }
  void scale(int k) { x = x * k; y = y * k; }
};
struct Trace {
  int id;
  ~Trace() { printf("dtor %d\n", id); }
};
int twice(int v) { calls = calls + 1; return v * 2; }
int pure(RootPair p) { return root_add(p.a, p.b) * 11; }
namespace inner {
int thrice(int v) { calls = calls + 1; return v * 3; }
} // namespace inner
} // namespace geo

int flat(int v) { return v + 1; }

// Reopened: `quad` must land in the SAME module as the block above.
namespace geo {
int quad(int v) { return twice(twice(v)); }
} // namespace geo

namespace {
int hidden(int v) { return v - 1; }
} // namespace

namespace outer { namespace mid { namespace deep {
int five(RootPair p) { return root_add(p.a, p.b) + 5; }
} } }

int main(int argc, char **argv) {
  int seed = argc;
  geo::Vec v;
  v.x = seed * 3;
  v.y = seed * 4;
  v.scale(seed + 1);
  geo::Trace t;
  t.id = seed * 9;
  RootPair pair;
  pair.a = seed * 2;
  pair.b = seed * 6;
  printf("%d %d %d %d %d %d %d %d\n", v.len2(), geo::twice(seed),
         geo::inner::thrice(seed), geo::quad(seed), geo::pure(pair), hidden(seed),
         outer::mid::deep::five(pair), flat(seed));
  printf("calls %d\n", geo::calls);
  return 0;
}

// Depth 1 reaches the crate root through `use super::*`...
// MOD:      mod geo {
// MOD-NEXT:     use super::*;
// MOD:          pub(crate) fn pure(v0: RootPair) -> i32 {
// ...and depth 3 through `use crate::*`, because `super` there is `mid`.
// MOD:      mod outer {
// MOD-NEXT:     pub(crate) mod mid {
// MOD-NEXT:         pub(crate) mod deep {
// MOD-NEXT:             use crate::*;
// MOD-NEXT:             pub(crate) fn five(v0: RootPair) -> i32 {

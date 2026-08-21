// W2.15 on the FR-40 whole-program index. A function template's emitted
// items are its INSTANTIATIONS, not the uninstantiated pattern, so the
// index must key one node per instantiation under the SAME suffixed
// symbol the importer emits (CSymbolNaming.h `templateArgSuffix` is the
// single source of truth both sides call — that is the CLAUDE.md
// byte-identity invariant), and must follow each instantiation's own
// body for its outgoing edges.
//
// This pins the two halves that were silently missing before W2.15 and
// are easy to regress independently: NODES (without the `collectItems`
// arm, `--emit=item-graph` on a template input reported only `c_main`
// and `printf`, so `--incremental`'s PORTING.md / emitrust-progress.json
// denominator silently understated the work) and EDGES out of an
// instantiation (without the `collectDependencies` arm, `scale_d` had a
// node but no `Calls` edge to the `add_d` its body invokes — an
// unreachable-looking item in the index).
//
// The two `scale` -> `add` edges are the load-bearing ones: they prove
// the inner call resolved per-instantiation (`scale_d` calls `add_d`,
// NOT `add_i32`), which is the whole point of monomorphizing.
// RUN: emitrust-cc --emit=item-graph %s -o - | FileCheck %s

extern "C" int printf(const char *, ...);

template <typename T>
T add(T a, T b) {
  return a + b;
}

template <typename T>
T scale(T a, int n) {
  T total = a;
  for (int i = 1; i < n; ++i)
    total = add(total, a);
  return total;
}

int main() {
  int si = scale(4, 3);
  double sd = scale(0.5, 3);
  printf("%d %.2f\n", si, sd);
  return 0;
}

// One node per instantiation, none for the pattern.
// CHECK-DAG: node add_i32 kind=function def=1
// CHECK-DAG: node add_d kind=function def=1
// CHECK-DAG: node scale_i32 kind=function def=1
// CHECK-DAG: node scale_d kind=function def=1
// CHECK-DAG: edge scale_i32 -> add_i32 kind=Calls
// CHECK-DAG: edge scale_d -> add_d kind=Calls
// CHECK-DAG: edge c_main -> scale_i32 kind=Calls
// CHECK-DAG: edge c_main -> scale_d kind=Calls

// RUN: emitrust-cc --emit=item-graph %s -o - | FileCheck %s --check-prefix=NOPATTERN
// The uninstantiated pattern is not an item and the instantiations never
// cross-link (`scale_d` must not call `add_i32`).
// NOPATTERN-NOT: node add kind=
// NOPATTERN-NOT: node scale kind=
// NOPATTERN-NOT: edge scale_d -> add_i32
// NOPATTERN-NOT: edge scale_i32 -> add_d

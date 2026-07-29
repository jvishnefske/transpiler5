// FR-40: the call structure of the item graph. Pins that every function
// becomes a `node ... kind=function` keyed on the name the importer emits
// (`main` -> `c_main`), that a direct call becomes a `Calls` edge, that
// direct recursion is a self-edge rather than being filtered away, that a
// call through a function pointer becomes a target-less `CallsIndirect`
// edge, that naming a function outside callee position is `TakesAddressOf`
// and NOT an extra `Calls`, that a `static` function's node key carries the
// per-TU tag, and that a prototype-only function still gets a node (with
// def=0) so a call into it is not silently dropped by the closure rule.
// RUN: emitrust-cc --emit=item-graph %s -o - | FileCheck %s

int leaf(int x);

static int square(int x) { return x * x; }

int countdown(int n) {
  if (n <= 0)
    return 0;
  return countdown(n - 1);
}

int apply(int (*fn)(int), int x) { return fn(x); }

int main(void) { return leaf(square(countdown(3))) + apply(square, 2); }

// Nodes come first, sorted by symbol; `leaf` is declared but never defined,
// so it is def=0 while everything else here is def=1.
// CHECK:      node apply kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-calls.c:22:5
// CHECK-NEXT: node c_main kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-calls.c:24:5
// CHECK-NEXT: node countdown kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-calls.c:16:5
// CHECK-NEXT: node leaf kind=function def=0 linkage=extern tu=0 loc={{.*}}item-graph-calls.c:12:5
// CHECK-NEXT: node tu0_square kind=function def=1 linkage=intern tu=0 loc={{.*}}item-graph-calls.c:14:12

// `apply` calls only through its parameter, so its sole call edge is the
// target-less indirect one; `fn` is a parameter, not an item, so it
// contributes no TakesAddressOf.
// CHECK-NEXT: edge apply -> ? kind=CallsIndirect

// `main` calls three functions directly and passes a fourth by address.
// CHECK-NEXT: edge c_main -> apply kind=Calls
// CHECK-NEXT: edge c_main -> countdown kind=Calls
// CHECK-NEXT: edge c_main -> leaf kind=Calls
// CHECK-NEXT: edge c_main -> tu0_square kind=Calls
// CHECK-NEXT: edge c_main -> tu0_square kind=TakesAddressOf

// Direct recursion is a self-edge.
// CHECK-NEXT: edge countdown -> countdown kind=Calls
// CHECK-NOT:  edge

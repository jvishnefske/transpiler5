// FR-40: the call structure of the item graph. Pins that every function
// becomes a `node ... kind=function` keyed on the name the importer emits
// (`main` -> `c_main`), that a direct call becomes a `Calls` edge, that
// direct recursion is a self-edge rather than being filtered away, that a
// call through a function pointer becomes a target-less `CallsIndirect`
// edge, that naming a function outside callee position is `TakesAddressOf`
// and NOT an extra `Calls` — for BOTH spellings, the bare-name decay and
// the explicit `&f` (the explicit form routes through the lvalue walk and
// was silently dropped until the FR-62 stage-B flip exposed it via
// c-testsuite 00089) — that a `static` function's node key carries the
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

int helper(int x) { return x + 1; }

int (*grab(void))(int) { return &helper; }

// Nodes come first, sorted by symbol; `leaf` is declared but never defined,
// so it is def=0 while everything else here is def=1.
// CHECK:      node apply kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-calls.c:25:5
// CHECK-NEXT: node c_main kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-calls.c:27:5
// CHECK-NEXT: node countdown kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-calls.c:19:5
// CHECK-NEXT: node grab kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-calls.c:31:7
// CHECK-NEXT: node helper kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-calls.c:29:5
// CHECK-NEXT: node leaf kind=function def=0 linkage=extern tu=0 loc={{.*}}item-graph-calls.c:15:5
// CHECK-NEXT: node tu0_square kind=function def=1 linkage=intern tu=0 loc={{.*}}item-graph-calls.c:17:12

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

// The EXPLICIT `&helper` is the same TakesAddressOf fact as the bare-name
// decay, and no Calls edge.
// CHECK-NEXT: edge grab -> helper kind=TakesAddressOf
// CHECK-NOT:  edge

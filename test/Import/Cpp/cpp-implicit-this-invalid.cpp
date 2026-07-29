// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/virtual-sibling.cpp 2>&1 | FileCheck %s --check-prefix=VIRTSIB
// RUN: not emitrust-cc --emit=import %t/out-of-line-missing.cpp -o - 2>&1 | FileCheck %s --check-prefix=NOBODY
// RUN: not emitrust-import-c %t/this-ref-arg.cpp 2>&1 | FileCheck %s --check-prefix=REFARG

// FR-47 located-rejection ledger: the implicit-`this` receiver sub-shapes
// that STAY rejected after the fix, each pinned to the diagnostic it
// actually produces, so a later wave that implements one of them has a
// documented starting point rather than discovering the wording fresh.
//
// The common thread is that none of these is blocked by the receiver
// itself: each is blocked by something FR-47 deliberately did not expand
// into (virtual dispatch, cross-TU definition supply) or, since FR-48, by
// a genuine aliasing conflict that has nothing to do with `this` being
// implicit.
// Writing the same call with an EXPLICIT receiver (`other.m()`) rejects
// identically, which is the evidence that the residual limitation is not
// an implicit-`this` limitation.

//--- virtual-sibling.cpp
// A sibling call through implicit `this` does NOT open virtual dispatch.
// The class is rejected at the virtual member's own declaration, in
// `collectRecordFields`, before any field or method imports — so the
// rejection lands on the declaration, not on the (perfectly ordinary)
// call site, and no half-imported class survives.
// VIRTSIB: virtual-sibling.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: virtual method
class Dispatching {
public:
  virtual int step() { return 1; }
  int run() { return step(); }
};

int use(void) {
  Dispatching d;
  return d.run();
}

//--- out-of-line-missing.cpp
// A method DECLARED in the class but never defined in any translation
// unit: FR-47's signature prepass registers an external stub, no
// definition ever fills it in, and `finalizeProject` rejects the dangling
// symbol. Pinned because the prepass is what creates that stub, making
// this the shape most likely to regress into a silently emitted
// body-less Rust method. Driven through `emitrust-cc` rather than
// `emitrust-import-c`, since the check is project-level (a single raw
// import legitimately leaves an external declaration behind, exactly as
// it does for a C prototype whose definition lives in another TU).
// NOBODY: error: unsupported: function 'Partial_missing' is referenced but not defined in any translation unit
class Partial {
public:
  int missing();
  int caller() { return missing(); }
};

int use(void) {
  Partial p;
  return p.caller();
}

//--- this-ref-arg.cpp
// Passing the receiver on to a sibling BY REFERENCE (`merge(*this)`).
// FR-48 landed reference parameters, so `Node &other` now maps to
// `&mut Node` and the rejection MOVED from the parameter's type to the
// call site — but it did not go away, and this case is now a SOUNDNESS
// rejection rather than a missing feature. `merge(*this)` borrows one
// object twice, once as the mutating method's receiver and once as the
// `Node &` argument; Rust admits at most one mutable borrow, so emitting
// it would produce a crate that does not compile. The same rule accepts
// the shared/shared spelling (a `const` method taking `const Node &`,
// pinned as an ACCEPT in cpp-references.cpp) and mirrors the same-base
// rejection the C pointer path already applies to `f(&a, &a)`.
// REFARG: this-ref-arg.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable reference argument and method receiver
class Node {
public:
  int merge(Node &other) { return v + other.v; }
  int self_merge() { return merge(*this); }
  int v;
};

int use(void) {
  Node n;
  n.v = 1;
  return n.self_merge();
}

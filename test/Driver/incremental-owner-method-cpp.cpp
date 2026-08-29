// FR-143, C++ side: descending into `emitrust.impl` must not invent evidence.
//
// The invariant this file pins has two halves, and the second is the one that
// matters. FR-143 made `collectEmittedSymbols` walk one level into every
// module-level symbol table, so an in-impl `emitrust.func` now counts as
// evidence that its symbol was emitted. In C that is exactly right -- FR-30
// owner promotion moves a graph item into an impl and the item keeps its
// graph key (test/Driver/incremental-owner-method-ported.c). In C++ there is
// a SECOND population inside those impls, the class member functions, and
// ItemGraph.h states they are deliberately not graph nodes at all. So:
//
//   1. a plain C++ member function changes NOTHING about the report -- it
//      matches no node, and it must not appear as an item or an off-graph
//      item either; and
//
//   2. its symbol cannot be mistaken for some OTHER item's evidence. The
//      in-impl func carries `cxxMethodMangledName`'s module-level symbol
//      (`Counter_bump`, renamed `counter_bump`), not the short `bump` the
//      emitter prints, and that name is unique across the module because a
//      free function claiming it is REJECTED as a redefinition. The second
//      half of this file is that adversarial program: if the collision were
//      ever allowed through, FR-143's descent would report a dropped free
//      function as `ported` and the ledger would lie about code that is not
//      in the crate.
//
// RUN: split-file %s %t
//
// RUN: emitrust-cc --emit=crate --incremental %t/plain.cpp -o %t/plain.crate \
// RUN:   --crate-type=lib 2>%t/plain.err
// RUN: FileCheck %s --check-prefix=PLAINRS --input-file=%t/plain.crate/src/lib.rs
// RUN: FileCheck %s --check-prefix=PLAIN \
// RUN:   --input-file=%t/plain.crate/emitrust-progress.json
// RUN: FileCheck %s --check-prefix=PLAINERR --allow-empty --input-file=%t/plain.err
//
// RUN: emitrust-cc --emit=crate --incremental %t/collide.cpp \
// RUN:   -o %t/collide.crate --crate-type=lib 2>%t/collide.err
// RUN: FileCheck %s --check-prefix=CLASHERR --input-file=%t/collide.err
// RUN: FileCheck %s --check-prefix=CLASH \
// RUN:   --input-file=%t/collide.crate/emitrust-progress.json
//
// RUN: emitrust-cc --emit=crate --incremental %t/dtor.cpp -o %t/dtor.crate \
// RUN:   --crate-type=lib 2>%t/dtor.err
// RUN: FileCheck %s --check-prefix=DTORRS --input-file=%t/dtor.crate/src/lib.rs
// RUN: FileCheck %s --check-prefix=DTOR \
// RUN:   --input-file=%t/dtor.crate/emitrust-progress.json

//--- plain.cpp
class Counter {
public:
  int n;
  int bump(int d) { n += d; return n; }
};

int use(int d) {
  Counter c;
  c.n = 0;
  return c.bump(d);
}

// The method IS in an impl -- so it IS in FR-143's widened emitted set.
// PLAINRS: impl Counter {
// PLAINRS-NEXT: pub fn bump(&mut self, d: i32) -> i32 {

// And the report is exactly what it was before FR-143: the class and the
// free function are the only two graph items, both ported, and the method
// contributes no row of its own to either bucket.
// PLAIN: "graph_items": 2
// PLAIN-NEXT: "ported": 2
// PLAIN-NEXT: "stubbed": 0
// PLAIN-NEXT: "dropped": 0
// PLAIN-NEXT: "missing": 0
// PLAIN-NEXT: "declared": 0
// PLAIN-NEXT: "off_graph_rejected": 0
// PLAIN-NEXT: "ported_permille": 1000
// PLAIN: "symbol": "Counter"
// PLAIN: "symbol": "use_"
// PLAIN-NOT: "symbol": "bump"
// PLAIN-NOT: "symbol": "counter_bump"
// PLAIN: "off_graph_items": []
// PLAINERR-NOT: warning:

//--- collide.cpp
class Counter {
public:
  int n;
  int bump(int d) { n += d; return n; }
};

// `Counter::bump`'s module symbol is `counter_bump`, and so is this
// function's. The importer refuses the second definition rather than
// emitting two items under one name.
int Counter_bump(int d) { return d + 1; }

int use(int d) {
  Counter c;
  c.n = 0;
  return c.bump(d) + Counter_bump(d);
}

// The exact rejection, pinned so a future rename scheme cannot quietly stop
// detecting the clash.
// CLASHERR: collide.cpp:10:5: warning: unsupported: conflicting definition of 'counter_bump' (already defined in another translation unit) (recovered: item dropped)
// CLASHERR: dropped 'counter_bump' [other] unsupported: conflicting definition of 'counter_bump'

// THE POINT: `counter_bump` is a graph item with NO emitted definition of
// its own, and the method's homonymous in-impl symbol does not rescue it.
// A rejection row outranks the emitted set in buildProgressReport, so the
// item reads `dropped` -- never `ported`.
// CLASH: "graph_items": 3
// CLASH-NEXT: "ported": 1
// CLASH-NEXT: "stubbed": 1
// CLASH-NEXT: "dropped": 1
// CLASH-NEXT: "missing": 0
// CLASH: "symbol": "counter_bump"
// CLASH-NEXT: "kind": "function"
// CLASH-NEXT: "status": "dropped"

//--- dtor.cpp
struct Res {
  int n;
  ~Res() { n = 0; }
};

int use(int d) {
  Res r;
  r.n = d;
  return r.n;
}

// The destructor lands in a TRAIT impl under the bare trait member name.
// DTORRS: impl Drop for Res {
// DTORRS-NEXT: fn drop(&mut self) {

// It is neither an item nor evidence for one: the record and the free
// function are the whole project, and no row is spelled `drop`.
// DTOR: "graph_items": 2
// DTOR-NEXT: "ported": 2
// DTOR-NEXT: "stubbed": 0
// DTOR-NEXT: "dropped": 0
// DTOR-NEXT: "missing": 0
// DTOR-NOT: "symbol": "drop"
// DTOR: "symbol": "Res"
// DTOR-NOT: "symbol": "drop"
// DTOR: "symbol": "use_"
// DTOR-NOT: "symbol": "drop"
// DTOR: "off_graph_items": []

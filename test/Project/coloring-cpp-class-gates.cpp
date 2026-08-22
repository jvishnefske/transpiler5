// RUN: emitrust-cc --emit=coloring %s -o - | FileCheck %s

// FR-118/FR-117: the two class-level screens FR-41's admissibility probe was
// measurably OUT OF STEP with, one in each direction. Both divergences were
// live on unpatched HEAD and are of the SCREEN-VS-IMPORTER kind -- the
// W2.17/W2.18/W2.19 "stale screen whose own test did not fail" lesson --
// not the plan-vs-IR kind FR-116 established the FR-41 probe is
// structurally blind to. This file is the test whose failure that lesson
// says must exist.
//
// FALSE GREEN, closed by adding the screen: a class with a user-declared
// COPY, MOVE or DELEGATING constructor is rejected by the importer (in
// `importCXXMethods`, which is why FR-118 had to undo the struct_def), yet
// `probeRecord` had no screen for it and colored the class `green
// reason=admissible`. A false green is the recoverable direction -- the
// import simply rejects later -- but it makes FR-49's root attribution
// credit the wrong construct.
//
// FALSE RED, closed on the IMPORTER side: a UNION carrying an overloaded
// operator was screened Red here while the importer ADMITTED it (the union
// path takes `collectUnionSlot` and never runs the W2.2 member-shape gate),
// which is the direction ItemColoring's doctrine forbids -- it colors a
// portable item Red, drags every caller down with no diagnostic, and starves
// FR-43's `--search`, whose roots must be Green|Yellow. Rather than narrow
// the screen, FR-117 made the importer raise the SAME
// `unsupported: overloaded operator` the struct path always raised, so the
// two now agree by REJECTION. `UnionOp` below is Red on both sides, which is
// what makes it a true red.
//
// A CONVERSION FUNCTION is deliberately NOT screened: FR-117 omits it and
// keeps the class importable, so screening it would mint a fresh false red.
// `Conv` pinned Green is that agreement.

/// Green: an ordinary default constructor is admitted.
struct Plain {
  int v;
  Plain() : v(0) {}
  int get() const { return v; }
};

/// `unsupported: copy/move/delegating constructor`, raised on the copy
/// constructor.
struct Copyable {
  int v;
  Copyable() : v(0) {}
  Copyable(const Copyable &o) : v(o.v) {}
};

/// Same gate through the MOVE constructor.
struct Movable {
  int v;
  Movable() : v(0) {}
  Movable(Movable &&o) : v(o.v) {}
};

/// Same gate through a DELEGATING constructor.
struct Delegating {
  int v;
  Delegating() : v(0) {}
  Delegating(int n) : Delegating() { v = n; }
};

/// A DEFAULTED copy constructor is rejected too (the importer's gate runs on
/// the broader user-DECLARED predicate; see
/// test/Import/Cpp/cpp-defaulted-ctor-invalid.cpp), so the screen must
/// mirror that and not stop at user-DEFINED ones.
struct DefaultedCopy {
  int v;
  DefaultedCopy() : v(0) {}
  DefaultedCopy(const DefaultedCopy &) = default;
};

/// Red on both sides now: the union's overloaded operator.
union UnionOp {
  int a;
  float b;
  int operator+(int x) const { return a + x; }
};

/// Green: a conversion function is omitted, not rejected.
struct Conv {
  int v;
  operator int() const { return v; }
};

int main() { return 0; }

// CHECK:      item Conv kind=record color=green reason=admissible
// CHECK-NEXT: item Copyable kind=record color=red reason=inadmissible construct=copy-move-constructor
// CHECK-NEXT: item DefaultedCopy kind=record color=red reason=inadmissible construct=copy-move-constructor
// CHECK-NEXT: item Delegating kind=record color=red reason=inadmissible construct=copy-move-constructor
// CHECK-NEXT: item Movable kind=record color=red reason=inadmissible construct=copy-move-constructor
// CHECK-NEXT: item Plain kind=record color=green reason=admissible
// CHECK-NEXT: item UnionOp kind=record color=red reason=inadmissible construct=overloaded-operator
// CHECK-NEXT: item c_main kind=function color=green reason=admissible
// CHECK-NEXT: tally green=3 yellow=0 red=5
// CHECK-NOT:  item

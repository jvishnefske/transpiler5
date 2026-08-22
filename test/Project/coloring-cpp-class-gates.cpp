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
// FALSE RED, closed on the IMPORTER side by FR-117 and then RE-DECIDED by
// FR-112: a UNION carrying an overloaded operator was screened Red here
// while the importer ADMITTED it (the union path takes `collectUnionSlot`
// and never runs the W2.2 member-shape gate) -- the direction
// ItemColoring's doctrine forbids. FR-117 closed it by making the union
// path REJECT like the struct path; FR-112 closed it the other way for
// good: BOTH operator gates are gone, an overloaded operator is OMITTED
// member-by-member (its uses are located rejections at the call), and the
// operator screen here went with them. `UnionOp` and `OpStruct` below are
// Green with the member omitted -- coloring an operator-carrying class Red
// now would be three FALSE REDS on FR-112's motivating repro, breaking
// FR-41's "false reds remain zero" contract and starving FR-43's
// `--search`, whose roots must be Green|Yellow.
//
// `VirtMethod` pins the boundary: FR-112 deliberately kept
// `unsupported: virtual method` CLASS-level (the vptr is real storage the
// emitted struct lacks -- `sizeof` folds 16 for a struct the emitter
// renders as 4 bytes), so the isVirtual screen must STAY, mirroring the
// importer gate in `collectRecordFields`. Green-with-omission next to
// red-for-virtual is exactly the line FR-112 draws.
//
// W2.26 re-drew the destructor/inheritance boundary and this file moved
// WITH it (the same stale-screen lesson, third application): a base class
// carrying a destructor no longer disqualifies `admitsSingleBaseAsField`
// (`Inherits` below is Green -- a merely-inheriting class gets a
// TRANSITIVE has_drop and imports), a class whose SOLE virtual member is
// its destructor is admissible (`SoleVirt` Green), and the virtual-method
// screen must EXCLUDE the destructor or a sole-virtual-dtor class stays
// falsely Red -- the FR-41 forbidden direction (`MultiVirt` pins the
// residual: any OTHER virtual method still rejects, and its construct tag
// must be `virtual-method`, not a double-tagged `destructor`). The wave
// also closes a false GREEN the base-screen removal would have widened: a
// field whose class carries a destructor -- its OWN or an inherited one
// -- is an importer rejection (`struct member of a class with a
// destructor`, the measured member-vs-base drop-order divergence), so the
// probe now screens droppy FIELDS transitively (`MemberBesideBase`,
// `HoldsInherits`).
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

/// Green since FR-112: the union path omits the operator like the struct
/// path does.
union UnionOp {
  int a;
  float b;
  int operator+(int x) const { return a + x; }
};

/// Green since FR-112: the operator is omitted, the siblings import.
struct OpStruct {
  int v;
  int get() const { return v; }
  int operator+(int x) const { return v + x; }
};

/// Red: the virtual-method gate stays class-level this wave.
struct VirtMethod {
  int v;
  virtual int get() const;
};

/// Green: a conversion function is omitted, not rejected.
struct Conv {
  int v;
  operator int() const { return v; }
};

/// Green: a destructor defined in this TU is admitted (W2.17), and stays
/// the anchor for the inheritance cases below.
struct DropBase {
  int v;
  ~DropBase() { v = 0; }
};

/// Green since W2.26: merely inheriting over a droppy base -- the importer
/// synthesizes a transitive has_drop, no impl Drop of its own.
struct Inherits : public DropBase {
  int extra;
};

/// Green since W2.26: the SOLE virtual member is the destructor.
struct SoleVirt {
  int v;
  virtual ~SoleVirt() { v = 0; }
};

/// Red, and under `virtual-method`: a virtual destructor beside ANOTHER
/// virtual method. The pin would read `construct=destructor` if the
/// virtual-method screen forgot to exclude the destructor (the coloring
/// method loop, unlike the importer's, does not `continue` after the
/// destructor branch).
struct MultiVirt {
  int v;
  virtual ~MultiVirt() { v = 0; }
  virtual int get() const;
};

/// Red: a droppy MEMBER beside a droppy base is the measured drop-order
/// divergence (~D ~M ~B native vs ~D ~B ~M image); the importer's member
/// gate stays, so the probe screens it too.
struct MemberBesideBase : public DropBase {
  DropBase again;
};

/// Red THROUGH the transitive predicate: the member's class has no
/// destructor of its own, only an inherited one.
struct HoldsInherits {
  Inherits h;
};

int main() { return 0; }

// CHECK:      item Conv kind=record color=green reason=admissible
// CHECK-NEXT: item Copyable kind=record color=red reason=inadmissible construct=copy-move-constructor
// CHECK-NEXT: item DefaultedCopy kind=record color=red reason=inadmissible construct=copy-move-constructor
// CHECK-NEXT: item Delegating kind=record color=red reason=inadmissible construct=copy-move-constructor
// CHECK-NEXT: item DropBase kind=record color=green reason=admissible
// CHECK-NEXT: item HoldsInherits kind=record color=red reason=inadmissible construct=destructor
// CHECK-NEXT: item Inherits kind=record color=green reason=admissible
// CHECK-NEXT: item MemberBesideBase kind=record color=red reason=inadmissible construct=destructor
// CHECK-NEXT: item Movable kind=record color=red reason=inadmissible construct=copy-move-constructor
// CHECK-NEXT: item MultiVirt kind=record color=red reason=inadmissible construct=virtual-method
// CHECK-NEXT: item OpStruct kind=record color=green reason=admissible
// CHECK-NEXT: item Plain kind=record color=green reason=admissible
// CHECK-NEXT: item SoleVirt kind=record color=green reason=admissible
// CHECK-NEXT: item UnionOp kind=record color=green reason=admissible
// CHECK-NEXT: item VirtMethod kind=record color=red reason=inadmissible construct=virtual-method
// CHECK-NEXT: item c_main kind=function color=green reason=admissible
// CHECK-NEXT: tally green=8 yellow=0 red=8
// CHECK-NOT:  item

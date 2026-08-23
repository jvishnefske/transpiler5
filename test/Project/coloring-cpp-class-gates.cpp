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
// credit the wrong construct. W2.23 NARROWED that gate -- the
// user-provided `T(const T&)` copy constructor now imports -- and this
// screen narrowed in lockstep (`Copyable` flipped Green, `NonConstCopy`
// pins the shape that stays Red), the same move-together discipline.
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
// `VirtMethod` pins the boundary, RE-DRAWN by W2.19a: the importer's
// class-level `virtual method` gate is GONE -- a class with virtual
// methods is admitted, calls on VALUES statically bind, and every
// dynamic channel (a pointer-shaped receiver, the polymorphic upcast,
// sizeof/alignof) is a CALL-SITE/expr-site located rejection with no
// record-level restatement. So the isVirtual screen went WITH the gate
// (the fourth application of the stale-screen lesson): keeping it would
// color every virtual-method class falsely Red, the FR-41 forbidden
// direction. The accepted residual is a false GREEN -- a method body
// containing `p->virt()` colors green here and rejects at import --
// which is exactly the under-approximation side coloring's
// false-red-only contract permits.
//
// W2.26 re-drew the destructor/inheritance boundary and this file moved
// WITH it (the same stale-screen lesson, third application): a base class
// carrying a destructor no longer disqualifies `admitsSingleBaseAsField`
// (`Inherits` below is Green -- a merely-inheriting class gets a
// TRANSITIVE has_drop and imports), a class whose SOLE virtual member is
// its destructor is admissible (`SoleVirt` Green -- and since W2.19a
// `MultiVirt`, a virtual dtor beside another virtual method, is Green
// too: only its dynamic USES reject, at the site). The wave
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

/// Green since W2.23: the user-provided `T(const T&)` copy constructor is
/// ADMITTED (it imports as an ordinary &mut-self method; see
/// test/Import/Cpp/copy-ctor.cpp), so the screen narrowed in lockstep with
/// the importer's gate -- keeping it would mint a false Red on every
/// admitted copy-ctor class, the probe's forbidden direction.
struct Copyable {
  int v;
  Copyable() : v(0) {}
  Copyable(const Copyable &o) : v(o.v) {}
};

/// Red: a copy ctor taking non-const `T&` stays outside the admitted
/// shape (`unsupported: copy constructor taking a non-const reference`).
struct NonConstCopy {
  int v;
  NonConstCopy() : v(0) {}
  NonConstCopy(NonConstCopy &o) : v(o.v) {}
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

/// Green since W2.19a: virtual methods no longer disqualify the class --
/// value uses statically bind; dynamic uses reject at the site.
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

/// Green since W2.19a: a virtual destructor beside ANOTHER virtual
/// method. Was the W2.26 residual Red; with the class-level virtual gate
/// gone the destructor-as-Drop admission (its dtor has a body) is all
/// that is probed, and it passes. Pinned so the screen removal cannot
/// half-happen: if either the dtor-exclusion or the screen itself crept
/// back, this line would go Red under `virtual-method` or `destructor`.
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
// CHECK-NEXT: item Copyable kind=record color=green reason=admissible
// CHECK-NEXT: item DefaultedCopy kind=record color=red reason=inadmissible construct=copy-move-constructor
// CHECK-NEXT: item Delegating kind=record color=red reason=inadmissible construct=copy-move-constructor
// CHECK-NEXT: item DropBase kind=record color=green reason=admissible
// CHECK-NEXT: item HoldsInherits kind=record color=red reason=inadmissible construct=destructor
// CHECK-NEXT: item Inherits kind=record color=green reason=admissible
// CHECK-NEXT: item MemberBesideBase kind=record color=red reason=inadmissible construct=destructor
// CHECK-NEXT: item Movable kind=record color=red reason=inadmissible construct=copy-move-constructor
// CHECK-NEXT: item MultiVirt kind=record color=green reason=admissible
// CHECK-NEXT: item NonConstCopy kind=record color=red reason=inadmissible construct=copy-move-constructor
// CHECK-NEXT: item OpStruct kind=record color=green reason=admissible
// CHECK-NEXT: item Plain kind=record color=green reason=admissible
// CHECK-NEXT: item SoleVirt kind=record color=green reason=admissible
// CHECK-NEXT: item UnionOp kind=record color=green reason=admissible
// CHECK-NEXT: item VirtMethod kind=record color=green reason=admissible
// CHECK-NEXT: item c_main kind=function color=green reason=admissible
// CHECK-NEXT: tally green=11 yellow=0 red=6
// CHECK-NOT:  item

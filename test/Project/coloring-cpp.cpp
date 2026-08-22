// FR-41: the C++ half of the admissibility probe.
//
// Five of the eleven screened constructs are C++-only and four of those five
// are RECORD-level, which is the interesting part: a C++ member function is
// not an item graph node (it is declared inside the record, and its emitted
// name depends on the class's assigned struct name), so a `virtual` method or
// a user-declared destructor has no node of its own to be Red. It is probed as
// part of the ENCLOSING RECORD instead — which is a node, and which is exactly
// the item that will not be emitted because of it.
//
// Three of the four record-level screens mirror rejections
// `CImporter::collectRecordFields` raises before it collects a single
// field: base classes, user-declared destructors, and virtual methods.
// (Overloaded operators were screened here too until FR-112 made the
// importer OMIT them, member by member, instead of rejecting the class --
// see `Eq` below.) The fifth screened construct, a reference type, is
// `CImporter::mapType`'s and is signature-level, so it costs its callers
// Red rather than Yellow.
//
// Three of those five screens are now POSITION- or SHAPE-dependent rather
// than unconditional, and BOTH halves of each are pinned below, because
// screening a construct the importer SUPPORTS is this probe's unsafe
// direction: it colors a portable item Red and drags every caller down with
// it, with no diagnostic and no later stage that could recover it (see
// test/Project/search-false-red.cpp). Since FR-48 a reference RETURN is
// screened and a reference PARAMETER is not. Since W2.17 a destructor
// DEFINED in this translation unit is admitted (it becomes `impl Drop`)
// and only the residual shapes are screened -- since W2.26 that includes
// a VIRTUAL destructor when it is the sole virtual member, and a base
// carrying a destructor (the derived class gets a transitive has_drop;
// see coloring-cpp-class-gates.cpp). Since W2.18 a SINGLE public
// non-virtual base is admitted (as an ordinary first field) and only the
// shapes with no such image are screened.
// RUN: emitrust-cc --emit=coloring %s -o - | FileCheck %s

/// Green: a plain data record is the same item whether it is spelled `struct`
/// or `class`, and non-virtual methods do not change that.
class Plain {
public:
  int value() const { return v; }
  int v;
};

/// `unsupported: base classes are not supported`. W2.18 MOVED this pin
/// forward rather than loosening it: the plain `class Derived : public
/// Plain` this used to spell is now ADMITTED (the base becomes a first
/// field named `base`), so the residual the screen must still catch is a
/// VIRTUAL base -- one shared subobject reached from several derived paths,
/// which a by-value field cannot represent.
class Derived : public virtual Plain {
public:
  int extra;
};

/// Green, and the reason the screen above had to be narrowed in lockstep
/// with the importer: a single public NON-VIRTUAL base over a data-only
/// class imports as `struct Flat { base: Plain, extra: i32 }` with every
/// inherited access flattened through the field (pinned in
/// test/Import/Cpp/inheritance.cpp). Colouring it Red would be a FALSE RED
/// -- unrecoverable, and strictly worse than not searching at all.
class Flat : public Plain {
public:
  int extra;
};

/// `unsupported: virtual method` — no vtable, no dynamic dispatch.
class Virt {
public:
  virtual int area() const;
  int side;
};

/// `unsupported: destructor with no definition in this translation unit`.
/// W2.17 admits a non-virtual destructor DEFINED in this TU (it becomes
/// `impl Drop`), so the screen is narrowed to the residual shapes; this one
/// is body-less, which stays out because an uncalled, undefined method is
/// silently dropped from emission and the `impl Drop` -- and every side
/// effect in it -- would vanish with it.
class Owned {
public:
  ~Owned();
  int handle;
};

/// Green since FR-112: an overloaded operator is OMITTED from the imported
/// class (every use is a located rejection at the call), so screening it
/// would color a portable item Red -- the probe's unsafe direction. The
/// virtual-method screen right above stays: FR-112 deliberately kept
/// `virtual` class-level (the vptr is storage the emitted struct lacks).
class Eq {
public:
  bool operator==(int rhs) const;
  int key;
};

/// A reference RETURN: signature-level, so no stub can be written either and
/// `calls_by_ref` is Red rather than Yellow.
const Plain &by_ref(const Plain &p);

/// Green: a reference PARAMETER is no longer screened at all. FR-48 made
/// `const T&`/`T&` parameters importable (as `&T`/`&mut T`), and screening a
/// SUPPORTED construct is this probe's unsafe direction -- it would color a
/// portable item Red and drag every caller down with it. Pinned green here
/// precisely because the screen it used to trip is still in the table for the
/// other reference positions.
int by_ref_param(const Plain &p);

int calls_by_ref() { return by_ref(Plain()).v + by_ref_param(Plain()); }

/// A Red record in the signature: same effect, reached through the record.
int uses_virt(Virt *v);

/// A Red record in the body only: Red, but still stubbable, so its caller is
/// Yellow.
int uses_owned() {
  Owned o;
  return o.handle;
}

int calls_uses_owned() { return uses_owned(); }

int main() { return calls_by_ref() + calls_uses_owned(); }

// CHECK:      item Derived kind=record color=red reason=inadmissible construct=base-class
// CHECK-NEXT: item Eq kind=record color=green reason=admissible
// CHECK-NEXT: item Flat kind=record color=green reason=admissible
// CHECK-NEXT: item Owned kind=record color=red reason=inadmissible construct=destructor
// CHECK-NEXT: item Plain kind=record color=green reason=admissible
// CHECK-NEXT: item Virt kind=record color=red reason=inadmissible construct=virtual-method
// CHECK-NEXT: item by_ref kind=function color=red reason=inadmissible construct=reference-type
// CHECK-NEXT: item by_ref_param kind=function color=green reason=admissible
// `calls_by_ref` is Red (its callee has no stub) but its OWN signature is
// clean, so it is stubbable and `c_main` above it is only Yellow — Red travels
// arbitrarily far, unstubbability exactly one hop.
// CHECK-NEXT: item c_main kind=function color=yellow reason=stub-callee via=calls_by_ref edge=Calls chain=c_main->calls_by_ref->by_ref construct=reference-type
// CHECK-NEXT: item calls_by_ref kind=function color=red reason=red-callee via=by_ref edge=Calls chain=calls_by_ref->by_ref construct=reference-type
// CHECK-NEXT: item calls_uses_owned kind=function color=yellow reason=stub-callee via=uses_owned edge=Calls chain=calls_uses_owned->uses_owned->Owned construct=destructor
// CHECK-NEXT: item uses_owned kind=function color=red reason=red-type via=Owned edge=BodyType chain=uses_owned->Owned construct=destructor
// CHECK-NEXT: item uses_virt kind=function color=red reason=red-type via=Virt edge=SigType chain=uses_virt->Virt construct=virtual-method
// CHECK-NEXT: tally green=4 yellow=2 red=7
// CHECK-NOT:  item

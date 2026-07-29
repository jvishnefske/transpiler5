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
// The four are the four unconditional rejections
// `CImporter::collectRecordFields` raises before it collects a single field:
// base classes, user-declared destructors, virtual methods, and overloaded
// operators. The fifth, a reference type, is `CImporter::mapType`'s and is
// signature-level, so it costs its callers Red rather than Yellow.
// RUN: emitrust-cc --emit=coloring %s -o - | FileCheck %s

/// Green: a plain data record is the same item whether it is spelled `struct`
/// or `class`, and non-virtual methods do not change that.
class Plain {
public:
  int value() const { return v; }
  int v;
};

/// `unsupported: base classes are not supported` — importing only the derived
/// class's own fields would silently lose the inherited data.
class Derived : public Plain {
public:
  int extra;
};

/// `unsupported: virtual method` — no vtable, no dynamic dispatch.
class Virt {
public:
  virtual int area() const;
  int side;
};

/// `unsupported: user-declared destructor` — no drop semantics are modeled.
class Owned {
public:
  ~Owned();
  int handle;
};

/// `unsupported: overloaded operator` — no operator-overload lowering.
class Eq {
public:
  bool operator==(int rhs) const;
  int key;
};

/// A reference PARAMETER: signature-level, so no stub can be written either
/// and `calls_by_ref` is Red rather than Yellow.
int by_ref(const Plain &p);

int calls_by_ref() { return by_ref(Plain()); }

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
// CHECK-NEXT: item Eq kind=record color=red reason=inadmissible construct=overloaded-operator
// CHECK-NEXT: item Owned kind=record color=red reason=inadmissible construct=destructor
// CHECK-NEXT: item Plain kind=record color=green reason=admissible
// CHECK-NEXT: item Virt kind=record color=red reason=inadmissible construct=virtual-method
// CHECK-NEXT: item by_ref kind=function color=red reason=inadmissible construct=reference-type
// `calls_by_ref` is Red (its callee has no stub) but its OWN signature is
// clean, so it is stubbable and `c_main` above it is only Yellow — Red travels
// arbitrarily far, unstubbability exactly one hop.
// CHECK-NEXT: item c_main kind=function color=yellow reason=stub-callee via=calls_by_ref edge=Calls chain=c_main->calls_by_ref->by_ref construct=reference-type
// CHECK-NEXT: item calls_by_ref kind=function color=red reason=red-callee via=by_ref edge=Calls chain=calls_by_ref->by_ref construct=reference-type
// CHECK-NEXT: item calls_uses_owned kind=function color=yellow reason=stub-callee via=uses_owned edge=Calls chain=calls_uses_owned->uses_owned->Owned construct=destructor
// CHECK-NEXT: item uses_owned kind=function color=red reason=red-type via=Owned edge=BodyType chain=uses_owned->Owned construct=destructor
// CHECK-NEXT: item uses_virt kind=function color=red reason=red-type via=Virt edge=SigType chain=uses_virt->Virt construct=virtual-method
// CHECK-NEXT: tally green=1 yellow=2 red=8
// CHECK-NOT:  item

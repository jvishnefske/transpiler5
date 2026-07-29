// FR-41: the no-stub side of the asymmetry, and the three consequences the
// single propagation rule produces.
//
// A rejected TYPE is DROPPED, not stubbed: its size, its fields, and its
// derives are load-bearing at every use and nothing can stand in for them. So
// type poison is transitive and turns every dependent RED, along `Field`,
// `SigType`, and `BodyType` alike. What differs between the type edges is not
// the dependent's own color but its effect on ITS OWN callers, and that is
// what the two halves below pin:
//
//  - `by_signature` names the Red type in its SIGNATURE. Its stub would have
//    to spell the missing type in its own parameter list, so no stub exists
//    either, and `calls_by_signature` is RED (reason=red-callee).
//  - `by_body` names the Red type only INSIDE its body. The body cannot be
//    written, so `by_body` is Red — Yellow would be a lie, since the function
//    does not emit — but its `int(void)` signature is untouched, so a stub
//    DOES exist and `calls_by_body` is only YELLOW.
//
// That is the whole reason `BodyType` is treated as a hard, Red-producing
// dependency rather than a soft one: the boundary a stub can be inserted at is
// the function's, not the statement's.
//
// The third thing pinned here is that UNSTUBBABILITY DOES NOT PROPAGATE, only
// Red does. `calls_by_signature` is Red — it calls a function that will not
// exist — but its OWN signature is `int(void)` and maps fine, so a stub
// replaces IT, and `c_main` above it is merely Yellow. Red therefore travels
// arbitrarily far while the "not even a stub" property stops after exactly one
// hop, which is what keeps a single unsupported type from reddening a whole
// program.
// RUN: emitrust-cc --emit=coloring %s -o - | FileCheck %s

// `_Atomic` is one of the two unconditional type rejections in
// `CImporter::mapType` — there is no atomics model in the single-threaded
// emitted program — so a record with an `_Atomic` field is a Red seed with no
// flow-sensitivity anywhere near it.
struct Atom {
  _Atomic int cell;
};

// Field poison: one hop.
struct Wrap {
  struct Atom atom;
  int tag;
};

// Field poison: two hops, so the chain has three links and still names the
// original construct.
struct Outer {
  struct Wrap wrap;
};

int by_signature(struct Outer *o) { return o->wrap.tag; }

int calls_by_signature(void) { return by_signature(0); }

int by_body(void) {
  struct Wrap local;
  local.tag = 7;
  return local.tag;
}

int calls_by_body(void) { return by_body(); }

int main(void) { return calls_by_signature() + calls_by_body(); }

// The seed, and the two records it poisons through `Field` edges. Note the
// chain lengthens by one link per hop and keeps naming `atomic-type`.
// CHECK:      item Atom kind=record color=red reason=inadmissible construct=atomic-type
// CHECK-NEXT: item Outer kind=record color=red reason=red-type via=Wrap edge=Field chain=Outer->Wrap->Atom construct=atomic-type
// CHECK-NEXT: item Wrap kind=record color=red reason=red-type via=Atom edge=Field chain=Wrap->Atom construct=atomic-type

// A Red type in the BODY: Red, but still stubbable...
// CHECK-NEXT: item by_body kind=function color=red reason=red-type via=Wrap edge=BodyType chain=by_body->Wrap->Atom construct=atomic-type
// A Red type in the SIGNATURE: Red and NOT stubbable.
// CHECK-NEXT: item by_signature kind=function color=red reason=red-type via=Outer edge=SigType chain=by_signature->Outer->Wrap->Atom construct=atomic-type

// Both callers are Red-or-Yellow according to their callee's stubbability, and
// the two blame chains splice cleanly onto the callee's own.
// CHECK-NEXT: item c_main kind=function color=yellow reason=stub-callee via=calls_by_signature edge=Calls chain=c_main->calls_by_signature->by_signature->Outer->Wrap->Atom construct=atomic-type
// CHECK-NEXT: item calls_by_body kind=function color=yellow reason=stub-callee via=by_body edge=Calls chain=calls_by_body->by_body->Wrap->Atom construct=atomic-type
// CHECK-NEXT: item calls_by_signature kind=function color=red reason=red-callee via=by_signature edge=Calls chain=calls_by_signature->by_signature->Outer->Wrap->Atom construct=atomic-type
// CHECK-NEXT: tally green=0 yellow=2 red=6
// CHECK-NOT:  item

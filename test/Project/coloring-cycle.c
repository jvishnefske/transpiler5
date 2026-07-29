// FR-41: the fixpoint terminates, and the blame chain does too, on a graph
// full of cycles.
//
// Three cycles at once:
//  - `ping`/`pong`, mutually recursive and BOTH poisoned from the same Red
//    callee, so the naive "record whoever poisoned me first" blame would have
//    them pointing at each other forever;
//  - `Node`, a self-referential record (a `Field` self-edge);
//  - `Ring`/`Loop`, two records referring to each other, both dragged Red by
//    a third.
//
// Termination is not an accident of the input: colors are a LEAST fixpoint of
// a monotone rule set over finite sets, and blame follows strictly decreasing
// poison RANK (least steps from an inadmissible seed), so no chain can revisit
// a node. Both `ping` and `pong` therefore blame `blocked` directly rather
// than each other, and neither chain runs through the cycle.
// RUN: emitrust-cc --emit=coloring %s -o - | FileCheck %s

struct Atom {
  _Atomic int cell;
};

// A self-referential record: a `Field` self-edge that the type walk must not
// loop on.
struct Node {
  struct Node *next;
  int value;
};

// A two-record cycle, both Red through the same seed.
struct Ring;
struct Loop {
  struct Ring *ring;
  struct Atom atom;
};
struct Ring {
  struct Loop *loop;
  struct Atom atom;
};

void blocked(void) { __asm__(""); }

int pong(int n);

int ping(int n) {
  blocked();
  return n <= 0 ? 0 : pong(n - 1);
}

int pong(int n) {
  blocked();
  return n <= 0 ? 1 : ping(n - 1);
}

int walk(struct Node *n) { return n->value; }

int main(void) { return ping(3) + walk(0); }

// The Red seed and the four records: `Node` is untouched by the seed and stays
// GREEN despite its self-edge, while `Loop` and `Ring` are each Red through
// their OWN `Field` edge to `Atom` (rank 1), not through each other (which
// would be rank 2) — the rank rule choosing the shortest explanation.
// CHECK:      item Atom kind=record color=red reason=inadmissible construct=atomic-type
// CHECK-NEXT: item Loop kind=record color=red reason=red-type via=Atom edge=Field chain=Loop->Atom construct=atomic-type
// CHECK-NEXT: item Node kind=record color=green reason=admissible
// CHECK-NEXT: item Ring kind=record color=red reason=red-type via=Atom edge=Field chain=Ring->Atom construct=atomic-type

// Both halves of the mutual recursion blame `blocked` at rank 1, not each
// other; neither chain enters the cycle.
// CHECK-NEXT: item blocked kind=function color=red reason=inadmissible construct=inline-asm
// `c_main` calls only `ping`, which is YELLOW rather than Red, so `c_main` is
// GREEN: Yellow is not a propagating property (see coloring-yellow.c).
// CHECK-NEXT: item c_main kind=function color=green reason=admissible
// CHECK-NEXT: item ping kind=function color=yellow reason=stub-callee via=blocked edge=Calls chain=ping->blocked construct=inline-asm
// CHECK-NEXT: item pong kind=function color=yellow reason=stub-callee via=blocked edge=Calls chain=pong->blocked construct=inline-asm
// CHECK-NEXT: item walk kind=function color=green reason=admissible
// CHECK-NEXT: tally green=3 yellow=2 red=4
// CHECK-NOT:  item

// FR-41: the coloring is a function of the project's CONTENT, not of the
// order its translation units were handed to the driver.
//
// This matters because the whole point of the coloring is to be the input of a
// reproducible whole-project search (FR-43): a search whose candidate set
// shifted with the argument order would not be reproducible at all. Two things
// could break it and both are exercised here:
//
//  - THE COLORS. They are a least fixpoint of a monotone rule set iterated to
//    saturation, so they cannot depend on visit order. `mixed` below is
//    poisoned from two different units at once to make a unit-order dependency
//    visible if one existed.
//  - THE BLAME CHAIN. This is the fragile half: "the item that poisoned me" is
//    ambiguous whenever several did, and recording whichever the iteration
//    reached first WOULD depend on order. `mixed` calls `blocked_a` and
//    `blocked_b`, equally good poisoners reached along the same edge kind, so
//    the choice can only come from the content (the smaller symbol,
//    `blocked_a`). Likewise `c_main` reaches its Red root along several
//    distinct paths of different lengths, so the shortest-rank rule is
//    exercised too.
//
// The two RUN lines below are the same project with the units swapped; the
// third asserts the outputs are byte-identical, which is the strongest form of
// the claim.
// RUN: emitrust-cc --emit=coloring %s %S/Inputs/coloring-determinism-other.c \
// RUN:   -I %S/Inputs -o %t.forward
// RUN: emitrust-cc --emit=coloring %S/Inputs/coloring-determinism-other.c %s \
// RUN:   -I %S/Inputs -o %t.reverse
// RUN: diff %t.forward %t.reverse
// RUN: FileCheck %s < %t.forward

#include "coloring-determinism.h"

/// Defined in the other unit; declared here so `main` can reach across.
int mixed(void);

void blocked_a(void) { __asm__(""); }

int calls_wide(void) { return wide(0); }

int main(void) {
  struct Plain p;
  p.value = plain_value(&p);
  return mixed() + calls_wide() + p.value;
}

// CHECK:      item Atom kind=record color=red reason=inadmissible construct=atomic-type
// CHECK-NEXT: item Held kind=record color=red reason=red-type via=Atom edge=Field chain=Held->Atom construct=atomic-type
// CHECK-NEXT: item Plain kind=record color=green reason=admissible
// CHECK-NEXT: item blocked_a kind=function color=red reason=inadmissible construct=inline-asm
// CHECK-NEXT: item blocked_b kind=function color=red reason=inadmissible construct=inline-asm

// c_main calls `calls_wide` (Red, but stubbable, since its own signature is
// clean) and `mixed` (Yellow, which does not propagate), so it is Yellow, and
// its chain runs through the Red one all the way to the seed in the OTHER
// unit's shared header.
// CHECK-NEXT: item c_main kind=function color=yellow reason=stub-callee via=calls_wide edge=Calls chain=c_main->calls_wide->wide->Held->Atom construct=atomic-type
// CHECK-NEXT: item calls_wide kind=function color=red reason=red-callee via=wide edge=Calls chain=calls_wide->wide->Held->Atom construct=atomic-type

// Two equally good poisoners along the same edge kind: the smaller symbol
// wins, in both unit orderings.
// CHECK-NEXT: item mixed kind=function color=yellow reason=stub-callee via=blocked_a edge=Calls chain=mixed->blocked_a construct=inline-asm
// CHECK-NEXT: item plain_value kind=function color=green reason=admissible
// CHECK-NEXT: item wide kind=function color=red reason=red-type via=Held edge=SigType chain=wide->Held->Atom construct=atomic-type
// CHECK-NEXT: tally green=2 yellow=2 red=6
// CHECK-NOT:  item

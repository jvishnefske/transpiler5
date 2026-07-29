// FR-43: the root candidate is FR-41's coloring, so the constructs the
// coloring already rules out never cost the search an import attempt.
//
// `_Atomic` is one of `CImporter::mapType`'s two unconditional type
// rejections, so `struct Atom` is a Red seed with no flow-sensitivity near it
// and `uses_atom`, which names it in its signature, is Red by type poison
// (FR-41: a dropped type has no stand-in). Both are excluded from the root
// candidate BEFORE any import runs. The search then probes once, learns
// nothing new -- the rejections it hears back are the ones it caused, marked
// `new=no`, and generating children from those would be circular -- and stops.
//
// The `why` token on an `excluded` line names which of three different pieces
// of work an item is waiting on; every exclusion here is `red`, and the other
// two are pinned in search-backtrack.cpp:
//
//   red      the coloring ruled it out. Support the construct its FR-41 blame
//            chain names (`--emit=coloring` prints that chain).
//   cascade  nothing is wrong with it; it depends on something that is out.
//   dropped  the search gave it up in response to a probe -- see
//            search-backtrack.cpp, where that is the whole story.
//
// RUN: emitrust-cc --emit=search %s -o - | FileCheck %s

struct Atom {
  _Atomic int cell;
};

// Red by SigType poison: a stub for it would have to spell `struct Atom` in
// its own parameter list, so it is not even stub-replaceable.
int uses_atom(struct Atom *a) { return 1; }

// Red in turn, because its callee is Red AND unstubbable (FR-41's
// `reason=red-callee`). Nothing is wrong with this function itself.
int calls_uses_atom(void) { return uses_atom(0); }

// Untouched by any of it.
int independent(int v) { return v + 1; }

int main(void) { return independent(2); }

// A record is a root too: FR-40 gives every record external linkage (the
// importer emits one struct per name program-wide), so all five items are
// roots here and only the two Green ones survive into the candidate.
// CHECK:      search items=5 roots=5 max-nodes=8
// CHECK-NEXT: root 0 admitted=2 excluded=3

// `calls_uses_atom` is Red but STUB-REPLACEABLE -- its own signature is
// `int(void)` and maps fine -- so excluding it still emits a stub, and the
// probe reports one stub against two dropped items.
// CHECK-NEXT: probe 0 outcome=ok ported=2 stubbed=1 rep-cost=0 dropped=2

// The rejections the probe reports back are the search's own exclusions:
// `new=no` marks them as facts it already had, which is exactly why no child
// is generated and `generated=1`.
// CHECK-NEXT: learn 0 rejected=Atom as=drop tag=search-excluded new=no
// CHECK-NEXT: learn 0 rejected=calls_uses_atom as=stub tag=search-excluded new=no
// CHECK-NEXT: learn 0 rejected=uses_atom as=drop tag=search-excluded new=no
// CHECK-NEXT: stop reason=exhausted
// CHECK-NEXT: best 0 ported=2 stubbed=1 rep-cost=0
// CHECK-NEXT: admitted c_main rep=default
// CHECK-NEXT: admitted independent rep=default
// CHECK-NEXT: excluded Atom why=red
// CHECK-NEXT: excluded calls_uses_atom why=red
// CHECK-NEXT: excluded uses_atom why=red
// CHECK-NEXT: summary probes=1 generated=1 pruned=0 improved=no

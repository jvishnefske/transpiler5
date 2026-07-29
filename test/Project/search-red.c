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
// `new=no` marks them as facts it already had, so none of them generates a
// child.
// CHECK-NEXT: learn 0 rejected=Atom as=drop tag=search-excluded new=no
// CHECK-NEXT: learn 0 rejected=calls_uses_atom as=stub tag=search-excluded new=no
// CHECK-NEXT: learn 0 rejected=uses_atom as=drop tag=search-excluded new=no

// FR-50's mandatory second probe: the BASELINE, the state that admits every
// item and excludes nothing, which is bit for bit the import a plain
// `--incremental` run performs. It is probed on every project whose coloring
// rules anything out, and it is what turns "the search is never worse than
// not searching" into a structural property rather than a hope -- `best` is a
// maximum over the probed states, and this state is always one of them.
// CHECK-NEXT: baseline 1 admitted=5 excluded=0

// And here the baseline EARNS its keep in the other direction: it does not
// win, and in not winning it CONFIRMS the coloring. FR-41 predicted `Atom`,
// `uses_atom` and `calls_uses_atom` were out; a real import of all five items
// rejects exactly those three and scores exactly what the root scored. The
// coloring is exact on this project, and that is now a measured fact rather
// than an argument from the header.
// CHECK-NEXT: probe 1 outcome=ok ported=2 stubbed=1 rep-cost=0 dropped=2
// CHECK-NEXT: learn 1 rejected=Atom as=drop tag=other new=yes

// `uses_atom` names `Atom` in its SIGNATURE. Since FR-50 the importer refuses
// to spell a type whose own import it rejected, so the rejection CASCADES
// rather than emitting `fn uses_atom(v0: &mut Atom)` against a struct nobody
// defines -- which is what it used to do, and which does not compile.
// CHECK-NEXT: learn 1 rejected=calls_uses_atom as=stub tag=other new=yes
// CHECK-NEXT: learn 1 rejected=uses_atom as=drop tag=rejected-type-cascade new=yes

// Every child the baseline proposes is a state the root already is, so all
// three are memoized away and the search stops with two probes spent.
// CHECK-NEXT: prune 2 from=1 drop=Atom why=memoized
// CHECK-NEXT: prune 3 from=1 drop=calls_uses_atom why=memoized
// CHECK-NEXT: prune 4 from=1 drop=uses_atom why=memoized
// CHECK-NEXT: stop reason=exhausted
// CHECK-NEXT: best 0 ported=2 stubbed=1 rep-cost=0
// CHECK-NEXT: admitted c_main rep=default
// CHECK-NEXT: admitted independent rep=default
// CHECK-NEXT: excluded Atom why=red
// CHECK-NEXT: excluded calls_uses_atom why=red
// CHECK-NEXT: excluded uses_atom why=red

// `>=baseline=yes` is FR-50's postcondition, printed on every run. It is
// `yes` by construction; a `no` would already have tripped the assertion in
// `frontierSearch`.
// CHECK-NEXT: summary probes=2 generated=5 pruned=3 improved=no >=baseline=yes

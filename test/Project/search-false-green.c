// FR-43: the LEARNED FACT -- an item FR-41's probe called Green that the real
// import refuses -- and the honest outcome that learning one does not always
// buy anything.
//
// `ItemColoring.h` is explicit that its probe UNDER-approximates, and it
// enumerates what it knowingly leaves Green rather than risk a false Red that
// no later stage could undo. A pointer-to-pointer parameter heads that list:
// `deref2` below is rejected because its bare `pp` truth-test escapes the
// cursor-parameter shape (C99-43 slice 1 admits the bounded `*pp`-only
// shape, `const char **` cursors and `main`'s `argv` import fine), so the
// syntax alone decides nothing and only an import attempt can settle it.
// That list is this search's work list, and this test is the smallest
// member of it.
//
// What the search does with the fact: the probe reports `deref2` (dropped,
// tag `ptr-to-ptr-shape-escape`) and `c_main` (stubbed, because it calls the
// item that is now gone), both marked `new=yes` -- admitted by the candidate
// and refused by the importer, which is exactly the false-Green signature.
// Children are generated for both, dropping the item and re-coloring around
// it.
//
// What it gets: nothing. All three candidates score 1 ported and 1 stub,
// because FR-42's per-item recovery had ALREADY reached the best partial
// module -- excluding a rejected item explicitly reproduces the same decision.
// `best 0` and `improved=no` say so, and that is the correct report rather
// than a defect: the children are worth generating because a rejected item can
// leave state behind that breaks a LATER item (see the rollback caveats in
// ImportCRecovery.cpp), and the only way to find out is to try. The cost of
// finding out is visible here as `probes=4` against a project that needed 1.
// RUN: emitrust-cc --emit=search %s -o - | FileCheck %s

int deref2(int **pp) { return pp ? **pp : 0; }

int plain(int v) { return v + 1; }

int main(void) {
  int x = 3;
  int *p = &x;
  return deref2(&p) + plain(1);
}

// CHECK:      search items=3 roots=3 max-nodes=8
// CHECK-NEXT: root 0 admitted=3 excluded=0
// CHECK-NEXT: probe 0 outcome=ok ported=1 stubbed=1 rep-cost=0 dropped=1

// The two false Greens, with the blocker tag the coloring probe could not
// have produced from the syntax alone.
// CHECK-NEXT: learn 0 rejected=c_main as=stub tag=other new=yes
// CHECK-NEXT: learn 0 rejected=deref2 as=drop tag=ptr-to-ptr-shape-escape new=yes

// A child per learned fact, marked `why=learned` rather than `why=blamed`:
// this candidate came from a rejection the import reported, not from
// attributing a whole-program failure.
// CHECK-NEXT: child 1 from=0 drop=deref2 cascade=1 why=learned
// CHECK-NEXT: probe 1 outcome=ok ported=1 stubbed=1 rep-cost=0

// Memoization: the same admitted set is reached by dropping the two items in
// either order, and it is imported once.
// CHECK:      prune {{[0-9]+}} from={{[0-9]+}} drop={{.*}} why=memoized

// The root wins on the tie, because nothing beat it and `best` only moves on
// a STRICT improvement.
// CHECK:      best 0 ported=1 stubbed=1 rep-cost=0
// CHECK-NEXT: admitted c_main rep=default
// CHECK-NEXT: admitted deref2 rep=default
// CHECK-NEXT: admitted plain rep=default
// CHECK-NEXT: summary probes=4 generated=5 pruned=1 improved=no

// And the crate is the crate the unsearched build already produced: when the
// search confirms the greedy answer, it changes nothing.
// RUN: emitrust-cc --emit=crate --incremental --search %s -o %t.searched
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.plain
// RUN: diff %t.searched/src/main.rs %t.plain/src/main.rs

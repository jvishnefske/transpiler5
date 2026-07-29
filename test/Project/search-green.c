// FR-43: a project that is entirely inside the subset, which is the case that
// has to cost as close to nothing as possible.
//
// The root candidate admits every item, the first probe imports the whole
// project, and there is nothing to learn: a probe with no rejections proposes
// no children, the frontier empties, and the search stops after ONE import.
// `improved=no` is the honest report of that -- the search did not beat the
// greedy answer here because there was nothing to beat.
//
// This is not a degenerate corner. It is the shape of every project in the
// FR-46 corpus whose coloring is exact, and it is why the default
// `--max-search-nodes=8` is affordable: the bound is what the search MAY
// spend, not what it does spend.
// RUN: emitrust-cc --emit=search %s -o - | FileCheck %s

static int scale(int v) { return v * 3; }

int step(int v) { return scale(v) + 1; }

int main(void) { return step(2) - 7; }

// CHECK:      search items=3 roots=2 max-nodes=8
// CHECK-NEXT: root 0 admitted=3 excluded=0
// CHECK-NEXT: probe 0 outcome=ok ported=3 stubbed=0 rep-cost=0 dropped=0

// No `learn` line at all: the import rejected nothing, so the coloring's
// verdict was exactly right and the search has no fact to act on.
// CHECK-NEXT: stop reason=exhausted
// CHECK-NEXT: best 0 ported=3 stubbed=0 rep-cost=0

// Every admitted item carries its representation. There is exactly one
// candidate per item today (the greedy Pass-A planning the importer already
// does), which is why `--search` cannot change the output of a project like
// this one -- and why the token exists at all, for the day FR-39's container
// split makes it a real choice.
// CHECK-NEXT: admitted c_main rep=default
// CHECK-NEXT: admitted step rep=default
// CHECK-NEXT: admitted tu0_scale rep=default

// FR-50's baseline probe costs this project NOTHING, and that is a property
// worth pinning rather than a happy accident. The baseline is the state that
// admits every item; the root already admits every item, because the coloring
// ruled nothing out; so the two states are the same state, there is no
// `baseline` line, and `probes=1` stands exactly where it stood. The
// safety net is paid for only by projects that have something to be wrong
// about.
// CHECK-NOT:  {{^}}baseline
// CHECK:      summary probes=1 generated=1 pruned=0 improved=no >=baseline=yes

// The crate a searched build emits is the crate an unsearched one emits: with
// nothing excluded, the import options are the ones they always were.
// RUN: emitrust-cc --emit=crate --incremental --search %s -o %t.searched
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.plain
// RUN: diff %t.searched/src/main.rs %t.plain/src/main.rs
// RUN: diff %t.searched/emitrust-progress.json %t.plain/emitrust-progress.json

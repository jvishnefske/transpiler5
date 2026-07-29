// FR-43: the case the frontier search exists for -- a recovering import that
// FAILS OUTRIGHT, repaired by giving up one item, and a choice between two
// repairs that the lexicographic score has to get right.
//
// The project is two translation units plus a shared header. The header
// declares `external_scale`, which the project calls and no translation unit
// defines: in the original program it comes from a prebuilt library outside
// the transpiled set. Everything here is inside the supported subset, so
// FR-41's coloring calls all five items Green and the root candidate admits
// the whole project.
//
// Under FR-42's per-item recovery alone the outcome is not a smaller crate but
// NO crate. Nothing is rejected item by item; the import dies at the end, in
// `finalizeProject`, with `function 'external_scale' is referenced but not
// defined in any translation unit`. That is a WHOLE-PROGRAM fact -- it is true
// of a SET of items, not of any one of them -- so no per-item recovery can
// attribute it, and `--incremental` exits nonzero with nothing written. The
// GREEDY run below asserts exactly that, because an improvement claim is only
// worth as much as the baseline it is measured against.
//
// The search repairs it in two more probes, and the second half of the story
// is the SCORE. Blame attribution proposes two candidates -- the missing
// `external_scale` itself, and its referrer `read_scaled` -- and both repairs
// work, so "the first one that happens to import" would be the wrong answer:
//
//   * dropping `external_scale` costs its declaration only. It is a function,
//     so FR-41's asymmetry leaves `read_scaled` merely Yellow: it is emitted
//     for real and calls an `unimplemented!()` stub. Four items port.
//   * dropping `read_scaled` costs a real function body. Three items port.
//
// Maximizing items emitted for real picks the first, and the trace records
// that it probed and scored both rather than stopping at the first success.
//
// RUN: emitrust-cc --emit=search %s %S/Inputs/search-backtrack-other.cpp \
// RUN:   -I %S/Inputs -o - | FileCheck %s
#include "search-backtrack.h"

int read_scaled(int value) { return external_scale(value) + 1; }

int triple(int value) { return value * 3; }

int main(void) { return read_scaled(1) + triple(2) + shared_util(3); }

// Five items, four of them definitions and therefore roots; the coloring
// objects to none of them, so the root candidate is the whole project.
// CHECK:      search items=5 roots=4 max-nodes=8
// CHECK-NEXT: root 0 admitted=5 excluded=0

// Probe 0 is the greedy answer, and it is not a partial port -- it is nothing.
// CHECK-NEXT: probe 0 outcome=failed ported=0 stubbed=0 rep-cost=0 dropped=0
// CHECK-NEXT: learn 0 failure=external_scale reason=unsupported: function 'external_scale' is referenced but not defined in any translation unit

// Probe 1 drops the undefined declaration. It is tried first because it is
// further from a root than its referrer is (the roots' only job in this
// algorithm: give up what the entry points lean on least).
// CHECK-NEXT: child 1 from=0 drop=external_scale cascade=1 why=blamed
// CHECK-NEXT: probe 1 outcome=ok ported=4 stubbed=1 rep-cost=0

// Probe 2 is the alternative repair, probed and scored even though probe 1
// already succeeded -- and it scores worse, which is why it must be.
// CHECK:      child 2 from=0 drop=read_scaled cascade=1 why=blamed
// CHECK-NEXT: probe 2 outcome=ok ported=3 stubbed=1 rep-cost=0

// CHECK:      best 1 ported=4 stubbed=1 rep-cost=0
// CHECK-NEXT: admitted c_main rep=default
// CHECK-NEXT: admitted read_scaled rep=default
// CHECK-NEXT: admitted shared_util rep=default
// CHECK-NEXT: admitted triple rep=default
// CHECK-NEXT: excluded external_scale why=dropped
// CHECK-NEXT: summary probes=3 generated=3 pruned=0 improved=yes

// The baseline, measured rather than remembered.
// RUN: not emitrust-cc --emit=crate --incremental %s \
// RUN:   %S/Inputs/search-backtrack-other.cpp -I %S/Inputs -o %t.greedy 2>&1 \
// RUN:   | FileCheck --check-prefix=GREEDY %s
// GREEDY: error: unsupported: function 'external_scale' is referenced but not defined in any translation unit

// And the searched build, which produces a crate where there was none: three
// real function bodies plus the stub the fourth compiles against.
// RUN: emitrust-cc --emit=crate --incremental --search %s \
// RUN:   %S/Inputs/search-backtrack-other.cpp -I %S/Inputs -o %t.searched
// RUN: FileCheck --check-prefix=CRATE %s < %t.searched/src/main.rs
// CRATE:      fn external_scale({{.*}}) -> i32 {
// CRATE-NEXT:     let {{.*}} = unimplemented!("excluded by the search state: item 'external_scale' is not admitted");
// CRATE:      fn read_scaled(
// CRATE:      fn triple(
// CRATE:      fn c_main(
// CRATE:      fn shared_util(

// The FR-44 report describes the searched crate, with the search's own
// blocker tag on the one item it gave up: four of the project's five items
// ported, against nothing at all without --search.
// RUN: FileCheck --check-prefix=JSON %s < %t.searched/emitrust-progress.json
// JSON:      "graph_items": 5,
// JSON-NEXT: "ported": 4,
// JSON:      "tag": "search-excluded"

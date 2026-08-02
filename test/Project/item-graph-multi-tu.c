// FR-40 across translation units. Two TUs each declare a file-`static`
// `tally` and a file-`static` `bump` with the SAME spelling; the graph must
// keep them apart, because the importer does — internal-linkage symbols are
// mangled with the per-TU tag `tu<i>_`, so the four items are
// `tu0_tally`/`tu0_bump` and `tu1_tally`/`tu1_bump`, each attributed to its
// own TU. Conversely the ONE external `shared_step`, prototyped in a shared
// header and defined in TU 0, must be a SINGLE node — attributed to the
// defining TU and located at the definition, not at either prototype — and
// the call to it from TU 1 must land on that same node.
// RUN: emitrust-cc --emit=item-graph %s %S/Inputs/item-graph-multi-tu-other.c -o - | FileCheck %s
#include "Inputs/item-graph-multi-tu.h"

static int tally;

static void bump(void) { tally = tally + 1; }

int shared_step(void) {
  bump();
  return tally;
}

// TU 1's only external item; nodes are ordered by symbol, not by TU.
// CHECK:      node TU0_TALLY kind=global def=1 linkage=intern tu=0 loc={{.*}}item-graph-multi-tu.c:13:12

// The external symbol is one node, attributed to TU 0 where it is DEFINED
// (line 17 here, not the header prototype and not TU 1's view of it).
// CHECK-NEXT: node TU1_TALLY kind=global def=1 linkage=intern tu=1 loc={{.*}}item-graph-multi-tu-other.c:7:12

// Same spelling, different TU, different node — and each carries its own
// TU index.
// CHECK-NEXT: node entry kind=function def=1 linkage=extern tu=1 loc={{.*}}item-graph-multi-tu-other.c:11:5
// CHECK-NEXT: node shared_step kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-multi-tu.c:17:5
// CHECK-NEXT: node tu0_bump kind=function def=1 linkage=intern tu=0 loc={{.*}}item-graph-multi-tu.c:15:13
// CHECK-NEXT: node tu1_bump kind=function def=1 linkage=intern tu=1 loc={{.*}}item-graph-multi-tu-other.c:9:13

// Each TU's static function touches only its OWN static global, and TU 1's
// call to the external symbol resolves onto the single shared node.
// CHECK-NEXT: edge entry -> shared_step kind=Calls
// CHECK-NEXT: edge entry -> tu1_bump kind=Calls
// CHECK-NEXT: edge shared_step -> tu0_bump kind=Calls
// CHECK-NEXT: edge shared_step -> TU0_TALLY kind=ReadsGlobal
// CHECK-NEXT: edge tu0_bump -> TU0_TALLY kind=ReadsGlobal
// CHECK-NEXT: edge tu0_bump -> TU0_TALLY kind=WritesGlobal
// CHECK-NEXT: edge tu1_bump -> TU1_TALLY kind=ReadsGlobal
// CHECK-NEXT: edge tu1_bump -> TU1_TALLY kind=WritesGlobal
// CHECK-NOT:  edge

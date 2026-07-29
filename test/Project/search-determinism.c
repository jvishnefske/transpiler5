// FR-43: the search's ANSWER is a function of the project's CONTENT, not of
// the order its translation units were handed to the driver, and not of how
// many times it is run.
//
// This is the property everything above the search depends on: a per-item
// ratchet (FR-44) that moved when a build system reordered its source list
// would be worthless, and a trace that answered "why is this item not in my
// crate?" differently on Tuesday would be worse than none.
//
// The ambiguity is planted deliberately. `missing_a` and `missing_b` are both
// declared and never defined, so the root candidate fails whole-program in
// `finalizeProject` -- which RETURNS AT THE FIRST undefined symbol it meets,
// and which one that is genuinely depends on the translation-unit order. Every
// choice the search makes downstream of that has to be broken on content
// alone, or the two runs diverge.
//
// It is broken by enumerating the whole FAILURE CLASS instead of chasing the
// one symbol reported: the graph knows every item the project merely
// DECLARES, and dropping all of them is one repair rather than one per symbol.
// That is what makes the child structure below identical in both orders -- and
// it is also what keeps the probe count linear when a real project calls ten
// library functions instead of two.
//
// EXACTLY ONE line of the trace is allowed to differ between the two orders,
// and it is not a decision: it is the verbatim text of the importer's own
// whole-program diagnostic, quoted rather than reworded, and it names whichever
// symbol `finalizeProject` reached first. The runs below assert that -- the
// full traces agree once that quoted line is set aside, and the ANSWER (the
// winning candidate, its score, and the admitted/excluded lists) agrees
// verbatim.
//
// RUN: emitrust-cc --emit=search %s %S/Inputs/search-determinism-other.c \
// RUN:   -I %S/Inputs -o %t.forward
// RUN: emitrust-cc --emit=search %S/Inputs/search-determinism-other.c %s \
// RUN:   -I %S/Inputs -o %t.reverse
// RUN: emitrust-cc --emit=search %s %S/Inputs/search-determinism-other.c \
// RUN:   -I %S/Inputs -o %t.again
//
// Repeated runs are identical outright.
// RUN: diff %t.forward %t.again
//
// Permuted runs are identical apart from the quoted diagnostic.
// RUN: grep -v ' failure=' %t.forward > %t.forward.decided
// RUN: grep -v ' failure=' %t.reverse > %t.reverse.decided
// RUN: diff %t.forward.decided %t.reverse.decided
//
// And the emitted CRATE contains the same ITEMS in both orders, which is the
// property the FR-44 ratchet above this rests on -- it keys items by symbol.
// The item ORDER is not compared, and deliberately not claimed: the emitter
// has always laid a module out in translation-unit order, so a permuted input
// permutes the file with or without a search. What the search must not do is
// change WHICH items are there, and that is what is checked.
// RUN: emitrust-cc --emit=crate --incremental --search %s \
// RUN:   %S/Inputs/search-determinism-other.c -I %S/Inputs -o %t.crate.forward
// RUN: emitrust-cc --emit=crate --incremental --search \
// RUN:   %S/Inputs/search-determinism-other.c %s -I %S/Inputs \
// RUN:   -o %t.crate.reverse
// RUN: grep '^fn ' %t.crate.forward/src/main.rs | sort > %t.items.forward
// RUN: grep '^fn ' %t.crate.reverse/src/main.rs | sort > %t.items.reverse
// RUN: diff %t.items.forward %t.items.reverse
//
// A repeated searched build is byte-identical outright, order included.
// RUN: emitrust-cc --emit=crate --incremental --search %s \
// RUN:   %S/Inputs/search-determinism-other.c -I %S/Inputs -o %t.crate.again
// RUN: diff %t.crate.forward/src/main.rs %t.crate.again/src/main.rs
// RUN: diff %t.crate.forward/emitrust-progress.json \
// RUN:   %t.crate.again/emitrust-progress.json
//
// RUN: FileCheck %s < %t.forward
#include "search-determinism.h"

int reads_a(int value) { return missing_a(value) + 1; }

int main(void) { return reads_a(1) + reads_b(2) + plain(3); }

// CHECK:      search items=6 roots=4 max-nodes=8
// CHECK-NEXT: root 0 admitted=6 excluded=0
// CHECK-NEXT: probe 0 outcome=failed ported=0 stubbed=0 rep-cost=0 dropped=0

// The class-complete repair: both undefined declarations at once, labelled
// `<first>+<how many more>` and probed FIRST because a partial repair of a
// whole-program failure usually only fails again.
// CHECK:      child 1 from=0 drop=missing_a+1 cascade=2 why=blamed
// CHECK-NEXT: probe 1 outcome=ok ported=4 stubbed=2 rep-cost=0

// The partial repairs are still generated and still scored -- giving up a
// caller instead of a declaration is a real alternative, just a worse one --
// which is why the winner is chosen by score rather than by arrival.
// CHECK:      best 1 ported=4 stubbed=2 rep-cost=0
// CHECK-NEXT: admitted c_main rep=default
// CHECK-NEXT: admitted plain rep=default
// CHECK-NEXT: admitted reads_a rep=default
// CHECK-NEXT: admitted reads_b rep=default
// CHECK-NEXT: excluded missing_a why=dropped
// CHECK-NEXT: excluded missing_b why=dropped
// CHECK:      improved=yes

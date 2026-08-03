// FR-62 (slice 2): the SCC condensation rule (R1). A non-trivial
// call-graph SCC is one emission unit: odd/even are mutually recursive and
// READ different singleton clusters (X and Y), so no writer-rule merge
// applies -- yet leaving X and Y as separate actors would make both
// functions cross-actor clients of each other's state inside one
// inseparable cycle. The planner merges the actors the cycle's combined
// Calls-closure footprint spans, keeps the smallest name, reports the
// note, and both members become plain arms of the merged actor.
//
// RUN: emitrust-cc --emit=actor-plan %s -o - 2>%t.err | FileCheck %s --match-full-lines
// RUN: FileCheck %s --check-prefix=WARN < %t.err

int x;
int y;

int even(int n);

int odd(int n) { return n ? even(n - 1) : x; }

int even(int n) { return n ? odd(n - 1) : y; }

// CHECK:      actor X globals=X,Y
// CHECK-NEXT: fn even role=arm actor=X
// CHECK-NEXT: fn odd role=arm actor=X
// CHECK-NEXT: note call cycle {even,odd} spans actors {X,Y}; merged into 'X'
// CHECK-NOT:  {{.+}}

// WARN: warning: actor plan: call cycle {even,odd} spans actors {X,Y}; merged into 'X'

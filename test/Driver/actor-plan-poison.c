// FR-62 (slice 2): the CallsIndirect universe-poison rule. `run` calls
// through a function pointer, so its write footprint is unknowable from
// the graph: the planner conservatively write-spans it over the WHOLE
// universe, which merges every actor it can see (here A and B) into one,
// with the poison named in the note. The merged plan is then internally
// consistent: with a single surviving actor, run's universe-wide footprint
// lies within one actor and it becomes an ordinary arm -- poison costs
// decomposition granularity, never soundness (E2 measured the poison rate
// at 2.3% EndToEnd / 0.7% kernel, so the cost is real but rare).
//
// RUN: emitrust-cc --emit=actor-plan %s -o - 2>%t.err | FileCheck %s --match-full-lines
// RUN: FileCheck %s --check-prefix=WARN < %t.err

int a;
int b;

void fa(void) { a = 1; }

void fb(void) { b = 2; }

void run(void (*f)(void)) { f(); }

// CHECK:      actor A globals=A,B
// CHECK-NEXT: fn fa role=arm actor=A
// CHECK-NEXT: fn fb role=arm actor=A
// CHECK-NEXT: fn run role=arm actor=A
// CHECK-NEXT: note function 'run' indirect-call poison spans actors {A,B}; merged into 'A'
// CHECK-NOT:  {{.+}}

// WARN: warning: actor plan: function 'run' indirect-call poison spans actors {A,B}; merged into 'A'

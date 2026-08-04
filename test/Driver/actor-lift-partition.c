// FR-62 F1b: the bin-local actor lift under --partition, driver-level
// pins (no cargo needed). The invariant this file pins: under
// `--link --partition` the driver lifts EXACTLY the actors whose whole
// cluster (owned globals, arms, cross clients) sits in the BINARY
// member's unit set, and demotes every other actor with the
// `spans workspace crates` warning -- never a lifted struct in a lib
// member, never a silently changed lib crate. Why: FR-59's globals
// invariant makes every actor cluster crate-local by construction, so
// bin-locality is decidable from the merged item graph's defining
// positions, and the lift's rewrite set is closed within the bin module
// (a lib member cannot reference into the binary crate).
//
// Three TUs: cluster X (extern global + arms) in a TU the globals
// invariant condenses into the bin crate (main reads x_count directly),
// cluster M (file-static + arms) in the main TU itself, and cluster Y
// (file-static + arms) wholly inside a lib-crate TU (main only calls).
//
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/actor-partition/binx/helper.c -o %t.x.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/actor-partition/liby/state.c -o %t.y.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
//
// RUN: rm -rf %t.ws
// RUN: emitrust-cc --link %t.x.o %t.y.o %t.main.o -o %t.ws --crate-name papp --emit=crate --partition 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN < %t.err
// WARN: warning: workspace partition: condensing
// WARN: warning: actor plan: demoted TU1_Y_TOTAL: spans workspace crates
//
// The bin member lifts both bin-local clusters (module order: X's
// global precedes main's file-static) and keeps the demoted actor's
// arms as plain cross-crate calls -- no trace of a Y actor:
// RUN: FileCheck %s --check-prefix=BIN < %t.ws/papp/src/main.rs
// BIN-NOT: Tu1YTotalActor
// BIN: struct XCountActor
// BIN: struct Tu2MCountActor
// BIN: x_count_actor.x_bump(3i32);
// BIN: tu2_m_count_actor.m_bump(5i32);
// BIN: y_add(10i32);
// BIN-NOT: Tu1YTotalActor
//
// The lib member is untouched by the lift: the demoted actor keeps
// today's thread-local form and no actor struct appears anywhere in it.
// RUN: FileCheck %s --check-prefix=LIB < %t.ws/liby/src/lib.rs
// LIB-NOT: Actor
// LIB: thread_local!
// LIB: static TU1_Y_TOTAL
// LIB: pub fn y_add
// LIB-NOT: Actor
//
// --actor-lift=false keeps the whole workspace demoted, with no
// demotion chatter (the lift was explicitly disabled, there is no plan
// to report on):
// RUN: rm -rf %t.off
// RUN: emitrust-cc --link %t.x.o %t.y.o %t.main.o -o %t.off --crate-name papp --emit=crate --partition --actor-lift=false 2>%t.off.err
// RUN: not grep "demoted" %t.off.err
// RUN: not grep "Actor" %t.off/papp/src/main.rs
// RUN: grep "thread_local" %t.off/papp/src/main.rs

extern int x_count;

void x_bump(int by);
int x_value(void);
void y_add(int v);
int y_total_now(void);

int printf(const char *, ...);

/* Actor cluster M: a file-static plus its arms in the bin TU itself. */
static int m_count = 0;

void m_bump(int by) { m_count += by; }
int m_value(void) { return m_count; }

int main(void) {
  x_bump(3);
  x_bump(4);
  m_bump(5);
  y_add(10);
  y_add(11);
  printf("x=%d direct=%d m=%d y=%d\n", x_value(), x_count, m_value(),
         y_total_now());
  return 0;
}

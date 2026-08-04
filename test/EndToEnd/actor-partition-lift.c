// REQUIRES: cargo
// FR-62 F1b, end to end: the bin-local actor lift under --partition
// changes packaging inside the binary member only, never behavior. The
// invariant this file pins: a partitioned workspace whose binary member
// LIFTS its bin-local actor clusters (one condensed-in TU cluster, one
// main-TU cluster) while the lib-side cluster demotes runs
// byte-identically to the clang-native binary AND to the unpartitioned
// single-crate link of the same objects (which, since F1a, lifts ALL
// three clusters) -- the byte-diff oracle is the only proof a lift did
// not miscompile; cargo build success cannot see it.
//
// Shim-built shards:
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/actor-ws/binx/helper.c -o %t.x.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/actor-ws/liby/state.c -o %t.y.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
//
// Partitioned workspace, built at the root:
// RUN: rm -rf %t.ws
// RUN: emitrust-cc --link %t.x.o %t.y.o %t.main.o -o %t.ws --crate-name actor_ws --emit=crate --partition --build 2>%t.err
// RUN: grep "demoted TU1_Y_TOTAL: spans workspace crates" %t.err
//
// The lift really happened in the binary member, and only there:
// RUN: grep "struct XCountActor" %t.ws/actor_ws/src/main.rs
// RUN: grep "struct Tu2MCountActor" %t.ws/actor_ws/src/main.rs
// RUN: grep "thread_local" %t.ws/liby/src/lib.rs
// RUN: not grep "Actor" %t.ws/liby/src/lib.rs
//
// Differential oracle against the clang-native binary:
// RUN: clang -std=c11 %S/Inputs/actor-ws/binx/helper.c %S/Inputs/actor-ws/liby/state.c %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.ws/target/release/actor_ws > %t.ws.out
// RUN: diff %t.native.out %t.ws.out
//
// Partitioning changes packaging, never behavior: the unpartitioned
// single-crate link (fully lifted since F1a) runs byte-identically.
// RUN: rm -rf %t.single
// RUN: emitrust-cc --link %t.x.o %t.y.o %t.main.o -o %t.single --crate-name actor_ws --emit=crate --build
// RUN: %t.single/target/release/actor_ws > %t.single.out
// RUN: diff %t.single.out %t.ws.out

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

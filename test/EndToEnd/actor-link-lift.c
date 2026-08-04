// REQUIRES: cargo
// FR-62 F1a: the actor lift under --link, end to end. Three TUs form a
// multi-actor cluster whose two libraries each define a file-static
// `counter` with the SAME spelling — the exact shape the (unit, symbol)
// keying exists for: the link-side plan retags each shard's statics by
// its link-line ordinal (the FR-58 merge's own alpha-rename), so the two
// statics land in DISTINCT actors (TallyActor owns lib1's counter with
// tally; Tu2CounterActor owns lib2's) instead of silently fusing. The
// oracles, in order of authority:
//  1. the lift-built linked crate's stdout byte-diffs against the
//     clang-built native binary (THE correctness oracle);
//  2. the linked crate root is BYTE-IDENTICAL to the joint lifted
//     --emit=rust of the same sources — sound here because every shard is
//     solo (no re-import group), so merged item order equals joint order;
//  3. the lift is real: the actor structs appear and no thread_local
//     survives for any lifted global.
//
// Per-TU shards through the shim:
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/actor-link-lift-lib1.c -o %t.lib1.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/actor-link-lift-lib2.c -o %t.lib2.o
//
// Link with the explicit lift and build the crate:
// RUN: emitrust-cc --actor-lift --link %t.main.o %t.lib1.o %t.lib2.o -o %t.crate --crate-name actor_link_lift --emit=crate --build
//
// Differential oracle against the clang-linked native binary:
// RUN: clang -std=c11 %s %S/Inputs/actor-link-lift-lib1.c %S/Inputs/actor-link-lift-lib2.c -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/actor_link_lift > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// Joint-vs-link byte identity, both sides lifted (the joint default is
// stage B's lift-on):
// RUN: emitrust-cc --emit=rust %s %S/Inputs/actor-link-lift-lib1.c %S/Inputs/actor-link-lift-lib2.c -o %t.joint.rs
// RUN: diff %t.joint.rs %t.crate/src/main.rs
//
// The lifted shape is real, with the same-named statics kept apart:
// RUN: FileCheck %s < %t.crate/src/main.rs
// RUN: not grep "thread_local" %t.crate/src/main.rs
//
// CHECK: struct SharedActor
// CHECK: struct TallyActor
// CHECK: struct Tu2CounterActor

int printf(const char *, ...);

int bump1(void);
int bump2(void);
int touch_shared(void);

int main(void) {
  printf("b1=%d\n", bump1());
  printf("b2=%d\n", bump2());
  printf("s=%d\n", touch_shared());
  printf("b1=%d\n", bump1());
  printf("b2=%d\n", bump2());
  return 0;
}

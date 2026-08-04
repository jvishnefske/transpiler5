// FR-62 F3: the `--actor-mode-map` per-actor mode surface — driver-only
// plumbing over the anchor op's existing per-actor mode field. One
// `<actor-name> <mode>` pair per line, where <actor-name> is the LIFTED
// actor type name (the name in the emitted struct, the thread-pass
// warnings and the anchor op — e.g. CounterActor). Pins six facts:
//  - PER-ACTOR OVERRIDE, both directions: under global
//    --actor-mode=threaded a `same-thread` entry EXCLUDES that actor from
//    the anchor list (it keeps the slice-4 struct shape, no
//    emitrust.actor_runtime op — exactly like a thread-pass veto), and
//    under the default global mode a `threaded` entry anchors exactly
//    that actor;
//  - MIXED FLAVORS ARE A LOCATED DRIVER ERROR: one crate carries one
//    actor_rt runtime flavor, so a per-module threaded+async mix fails in
//    the driver, in the emission backstop's wording
//    (test/Target/Rust/actor-runtime-mixed-modes-invalid.mlir stays
//    untouched as backstop);
//  - an actor name matching no lifted actor is a WARNING and the entry is
//    ignored (fail-toward-noop);
//  - a malformed line (wrong field count, unknown mode) is a clean error
//    naming the line, in --actor-map's wording style;
//  - a reifying (threaded/async) entry composes exactly like the global
//    --actor-mode: the lift is a mandatory precondition and the emission
//    modes are gated the same way;
//  - under --link a reifying entry reifies nothing (FR-62 F1a: the link
//    lift is same-thread only; actor mode under link is a recorded later
//    stage) and must SAY so, mirroring --actor-mode's link warning.
//
// Exclusion under a global reifying mode: CounterActor threads,
// TotalActor stays a plain lifted struct.
// RUN: echo "TotalActor same-thread" > %t.same.map
// RUN: emitrust-cc --actor-mode=threaded --actor-mode-map %t.same.map --emit=mlir %s -o - \
// RUN:   | FileCheck %s --check-prefix=SAME
// SAME-NOT: emitrust.actor_runtime @TotalActor
// SAME:     emitrust.actor_runtime @CounterActor mode =  threaded
// SAME-NOT: emitrust.actor_runtime @TotalActor
//
// Override in the other direction: the global mode is the default
// (same-thread), the map threads exactly one actor.
// RUN: echo "CounterActor threaded" > %t.one.map
// RUN: emitrust-cc --actor-mode-map %t.one.map --emit=mlir %s -o - \
// RUN:   | FileCheck %s --check-prefix=ONE
// ONE-NOT: emitrust.actor_runtime @TotalActor
// ONE:     emitrust.actor_runtime @CounterActor mode =  threaded
// ONE-NOT: emitrust.actor_runtime @TotalActor
//
// Threaded + async in one module is a located driver error (module loc).
// RUN: echo "TotalActor async" > %t.mix.map
// RUN: not emitrust-cc --actor-mode=threaded --actor-mode-map %t.mix.map --emit=mlir %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=MIX
// MIX: actor-mode-map.c:1:1: error: actor 'TotalActor' mode disagrees with actor 'CounterActor': one crate carries one actor_rt runtime flavor
//
// An unknown actor name warns and is ignored: nothing anchors, the run
// still succeeds.
// RUN: echo "GhostActor threaded" > %t.ghost.map
// RUN: emitrust-cc --actor-mode-map %t.ghost.map --emit=mlir %s -o %t.ghost.mlir 2>&1 \
// RUN:   | FileCheck %s --check-prefix=GHOST
// RUN: FileCheck %s --check-prefix=NOANCHOR < %t.ghost.mlir
// GHOST: warning: --actor-mode-map: actor 'GhostActor' matches no lifted actor; entry ignored
// NOANCHOR-NOT: emitrust.actor_runtime
//
// Malformed lines: wrong field count, then an unknown mode.
// RUN: echo "CounterActor" > %t.bad1.map
// RUN: not emitrust-cc --actor-mode-map %t.bad1.map --emit=mlir %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=BAD1
// BAD1: error: malformed --actor-mode-map line: 'CounterActor' (expected '<actor-name> <mode>')
// RUN: echo "CounterActor sometimes" > %t.bad2.map
// RUN: not emitrust-cc --actor-mode-map %t.bad2.map --emit=mlir %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=BAD2
// BAD2: error: malformed --actor-mode-map line: 'CounterActor sometimes' (mode must be 'threaded', 'async' or 'same-thread')
//
// A reifying entry requires the actor lift, --actor-mode's exact posture.
// RUN: not emitrust-cc --actor-mode-map %t.one.map --actor-lift=false --emit=rust %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=LIFT
// LIFT: error: --actor-mode-map mode 'threaded' requires the actor lift (remove --actor-lift=false): ownership lift is a mandatory precondition of threaded mode
//
// A reifying entry is gated to the lowered emission modes, like
// --actor-mode.
// RUN: not emitrust-cc --actor-mode-map %t.one.map --emit=import %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=GATE
// GATE: error: --actor-mode-map is only valid with --emit=mlir, --emit=rust or --emit=crate
//
// Under --link the lift is same-thread only: a reifying map entry
// reifies nothing and must say so; the link still succeeds.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.o
// RUN: emitrust-cc --actor-mode-map %t.one.map --link %t.o --emit=rust -o %t.link.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=LINKWARN
// LINKWARN: warning: --actor-mode-map under --link reifies nothing: actor mode under --link is a later stage (lifted actors stay same-thread)

int printf(const char *, ...);

int counter;
int total;

int bump(void) {
  counter += 1;
  return counter;
}

int add(int x) {
  total += x;
  return total;
}

int main(void) {
  printf("b=%d t=%d\n", bump(), add(2));
  printf("b=%d t=%d\n", bump(), add(3));
  return 0;
}

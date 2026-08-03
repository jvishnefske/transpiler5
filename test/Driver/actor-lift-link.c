// FR-62 slice 4 (stage A), DEMOTION RULE 5: `--actor-lift --link` demotes
// EVERY actor — the FR-58/FR-59 interaction (who owns a cross-shard
// actor's struct, how partitioned crates share it) is a recorded later
// stage, and recovery modes must never silently emit a half-lifted merge.
// The pins: one demote warning per planned actor (from the shards' stored
// FR-57d graphs, the same source --emit=actor-plan reads), the merged
// crate root still emits, and it keeps today's thread-local form (the
// global's accessor block survives, no actor struct appears).
//
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.o
// RUN: emitrust-cc --actor-lift --link %t.o --emit=rust -o %t.rs 2> %t.err
// RUN: FileCheck %s --check-prefix=WARN < %t.err
// RUN: FileCheck %s --check-prefix=RUST < %t.rs
//
// WARN: warning: actor plan: demoted COUNTER: --link actor lift is a later stage (demote-all under link)
//
// RUST:     thread_local!
// RUST-NOT: struct CounterActor

int counter;

int bump(void) {
  counter += 1;
  return counter;
}

int main(void) { return bump(); }

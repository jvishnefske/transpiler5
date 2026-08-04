// FR-62 F1a: an EXPLICIT `--actor-lift --link` LIFTS the merged module —
// the plan comes from the shards' stored FR-57d graphs (the same source
// --emit=actor-plan reads), the attributes attach to the MERGED module
// (the merge strips shard metadata, so attachment is post-merge by
// construction), and the lift pass runs post-merge exactly as it runs
// post-pipeline on the source path. The pins:
//  - the linked crate root carries the lifted actor (struct CounterActor,
//    no thread_local) with a SILENT stderr — nothing demoted, nothing to
//    report;
//  - it is byte-identical to the joint lifted --emit=rust of the same
//    source (a solo shard, so the joint-vs-link identity pin is sound);
//  - the no-metadata NEGATIVE: a shard artifact carrying no FR-57d
//    item-graph metadata demotes every actor with a note and keeps
//    today's thread_local form — fail-toward-current, never a failed
//    link and never a half-lifted merge.
//
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.o
// RUN: emitrust-cc --actor-lift --link %t.o --emit=rust -o %t.rs 2> %t.err
// RUN: FileCheck %s --check-prefix=QUIET --allow-empty < %t.err
// RUN: FileCheck %s --check-prefix=RUST < %t.rs
// RUN: not grep "thread_local" %t.rs
//
// QUIET-NOT: {{.}}
// RUST: struct CounterActor
//
// Byte identity with the joint lifted root:
// RUN: emitrust-cc --emit=rust %s -o %t.joint.rs
// RUN: diff %t.joint.rs %t.rs
//
// No-metadata negative: a bare post-pipeline module (no shard metadata at
// all) stands in for an artifact that predates FR-57d.
// RUN: emitrust-cc --actor-lift=false --emit=mlir %s -o %t.bare.mlir
// RUN: emitrust-cc --actor-lift --link %t.bare.mlir --emit=rust -o %t.bare.rs 2> %t.bare.err
// RUN: FileCheck %s --check-prefix=NOMETA < %t.bare.err
// RUN: FileCheck %s --check-prefix=BARE < %t.bare.rs
//
// NOMETA: warning: actor plan: --link lift demoted every actor: shard '{{.*}}.bare.mlir' carries no item-graph metadata (artifact predates FR-57d?)
// BARE:     thread_local!
// BARE-NOT: struct CounterActor

int counter;

int bump(void) {
  counter += 1;
  return counter;
}

int main(void) { return bump(); }

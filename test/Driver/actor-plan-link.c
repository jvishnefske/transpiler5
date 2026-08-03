// FR-62 (slice 2): `--link --emit=actor-plan` plans from the shards'
// STORED FR-57d item-graph texts alone -- a pure artifact query in the
// mold of the per-shard `--emit=item-graph` dump (no merge, no re-import,
// no C parsed). Pins two link-specific facts:
//  - INTERNAL-LINKAGE KEYING IS (unit, symbol): both TUs spell a
//    file-static `local_count`, and each per-TU shard graph tags it
//    `TU0_...`; the planner retags by link-line ordinal (the FR-58 merge's
//    own alpha-rename), so the two statics stay DISTINCT plan elements --
//    main's TU0_LOCAL_COUNT is its own actor while lib's TU1_LOCAL_COUNT
//    clusters with SHARED through tick's co-access.
//  - EXTERNAL SYMBOLS UNIFY across shards: `shared` (def in lib, extern
//    declaration in main) is ONE universe element, not two.
// The plan must also be byte-identical across two invocations (stored
// artifacts in, deterministic plan out).
//
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/actor-plan-link-lib.c -o %t.lib.o
//
// RUN: emitrust-cc --link %t.main.o %t.lib.o --emit=actor-plan -o %t.plan
// RUN: emitrust-cc --link %t.main.o %t.lib.o --emit=actor-plan -o %t.plan2
// RUN: diff %t.plan %t.plan2
// RUN: FileCheck %s --match-full-lines < %t.plan

static int local_count;

extern int shared;

int tick(void);

void bump_local(void) { local_count += 1; }

int main(void) { return tick() + local_count; }

// CHECK:      actor SHARED globals=SHARED,TU1_LOCAL_COUNT
// CHECK-NEXT: actor TU0_LOCAL_COUNT globals=TU0_LOCAL_COUNT
// CHECK-NEXT: fn bump_local role=arm actor=TU0_LOCAL_COUNT
// CHECK-NEXT: fn c_main role=driver actor=-
// CHECK-NEXT: fn tick role=arm actor=SHARED
// CHECK-NOT:  {{.+}}

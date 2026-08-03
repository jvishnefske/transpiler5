// FR-60: `emitrust-cc --link --emit=ratchet` generalizes the c-testsuite
// ledger to a per-project ratchet manifest: admitted-item counts (per
// shard, keyed by source path relative to the shards' common directory
// prefix so the manifest is checkout-portable, plus a total), the FR-59
// partition facts (crate count + condensation-warning count; 1/0 without
// --partition), and the per-tag rejection snapshot. `--ratchet-baseline`
// compares against a committed manifest with the c-testsuite ratchet's
// direction rules: an admitted SHRINK (total, per shard, or a vanished
// shard) or a condensation GROWTH is a FAILURE; admitted growth passes and
// the freshly written manifest IS the update.
//
// Reuses the FR-59 globals-invariant project: ga defines a global gb
// reads, so --partition condenses (1 warning, 2 crates).
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-partition-global/ga/def.c -o %t.a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-partition-global/gb/use.c -o %t.b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
//
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o --emit=ratchet --partition --crate-name gapp -o %t.manifest 2>/dev/null
// RUN: FileCheck %s < %t.manifest
// CHECK: emitrust ratchet manifest v1
// CHECK-NEXT: admitted-total: 4
// CHECK-NEXT: rejected-total: 0
// CHECK-NEXT: crates: 2
// CHECK-NEXT: condensation-warnings: 1
// CHECK-NEXT: shard '{{.*}}ga/def.c': admitted=2 rejected=0
// CHECK-NEXT: shard '{{.*}}gb/use.c': admitted=1 rejected=0
// CHECK-NEXT: shard 'link-ratchet.c': admitted=1 rejected=0
//
// Ratchet direction rules. Same manifest as baseline: pass.
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o --emit=ratchet --partition --crate-name gapp --ratchet-baseline %t.manifest -o %t.same 2>/dev/null
// RUN: diff %t.manifest %t.same
//
// SHRINK fails: a baseline claiming MORE admitted items than the build
// now produces is a regression.
// RUN: sed "s/admitted-total: 4/admitted-total: 5/" %t.manifest > %t.bigger
// RUN: not emitrust-cc --link %t.a.o %t.b.o %t.main.o --emit=ratchet --partition --crate-name gapp --ratchet-baseline %t.bigger -o %t.out 2>%t.err
// RUN: FileCheck %s --check-prefix=SHRINK < %t.err
// SHRINK: error: ratchet regression: admitted-total shrank from 5 to 4
//
// A vanished shard fails even when the totals happen to balance:
// RUN: sed "/gb.use.c/a shard 'gone.c': admitted=0 rejected=0" %t.manifest > %t.ghost
// RUN: not emitrust-cc --link %t.a.o %t.b.o %t.main.o --emit=ratchet --partition --crate-name gapp --ratchet-baseline %t.ghost -o %t.out2 2>%t.err2
// RUN: FileCheck %s --check-prefix=GHOST < %t.err2
// GHOST: error: ratchet regression: shard 'gone.c' is in the baseline but not in this build
//
// Condensation growth fails:
// RUN: sed "s/condensation-warnings: 1/condensation-warnings: 0/" %t.manifest > %t.tighter
// RUN: not emitrust-cc --link %t.a.o %t.b.o %t.main.o --emit=ratchet --partition --crate-name gapp --ratchet-baseline %t.tighter -o %t.out3 2>%t.err3
// RUN: FileCheck %s --check-prefix=CONDENSE < %t.err3
// CONDENSE: error: ratchet regression: condensation-warnings grew from 0 to 1
//
// GROWTH passes and the written manifest is the update:
// RUN: sed "s/admitted-total: 4/admitted-total: 3/" %t.manifest > %t.smaller
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o --emit=ratchet --partition --crate-name gapp --ratchet-baseline %t.smaller -o %t.updated 2>%t.note
// RUN: FileCheck %s --check-prefix=GROWTH < %t.note
// GROWTH: ratchet improvement: admitted-total grew from 3 to 4
// RUN: diff %t.manifest %t.updated
//
// Without --partition the crate facts are the single-crate identity.
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o --emit=ratchet -o - 2>/dev/null | FileCheck %s --check-prefix=SINGLE
// SINGLE: crates: 1
// SINGLE-NEXT: condensation-warnings: 0
//
// Gate: the manifest is computed from shard artifacts.
// RUN: not emitrust-cc --emit=ratchet %s -o - 2>&1 | FileCheck %s --check-prefix=GATE
// GATE: error: --emit=ratchet requires --link

int geta(void);
int getb(void);

int printf(const char *, ...);

int main(void) {
  printf("a=%d b=%d\n", geta(), getb());
  return 0;
}

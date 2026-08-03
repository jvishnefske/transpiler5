// FR-60: `emitrust-cc --link --emit=rejection-report` aggregates the
// FR-57d shard ledgers into the queryable per-construct report that ranks
// what semantic work buys the most frontier. Grouping is by the
// `classifyBlocker` TAG (the normalization table the ledger already
// records, shared verbatim with the RealWorld survey — spike-verified:
// every shard entry carries both the free-text diagnostic and the tag);
// within a tag the distinct diagnostic WORDINGS are tabulated with
// single-quoted spans normalized to '<name>' so per-symbol variants
// collapse. Output is deterministic: tags ranked by item count then name,
// wordings by count then text, each tag citing its first location. The
// report reads the ledgers AS RECORDED — no merge, no re-import — so it
// stays a pure artifact query that scales to a kernel link line.
//
// Two rejecting TUs and one clean TU:
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-report/mem.c -o %t.mem.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-report/vol.c -o %t.vol.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
//
// RUN: emitrust-cc --link %t.mem.o %t.vol.o %t.main.o --emit=rejection-report -o - | FileCheck %s
//
// `other` leads (2 items across 2 TUs); the singletons follow in tag
// order; the clean TU contributes nothing.
// CHECK: rejection report: 5 rejected items in 2 of 3 translation units
// CHECK-NEXT: rank 1. other  items=2  tus=2
// CHECK-NEXT:   first: {{.*}}mem.c:{{[0-9]+}}:{{[0-9]+}}
// CHECK-NEXT:   1x unsupported: void pointer parameter
// CHECK-NEXT:   1x unsupported: volatile-qualified type
// CHECK-NEXT: rank 2. dynamic-memory  items=1  tus=1
// CHECK-NEXT:   first: {{.*}}mem.c:{{[0-9]+}}:{{[0-9]+}}
// CHECK-NEXT:   1x unsupported: allocation size is not a compile-time constant
// CHECK-NEXT: rank 3. libc:qsort  items=1  tus=1
// CHECK-NEXT:   first: {{.*}}mem.c:{{[0-9]+}}:{{[0-9]+}}
// CHECK-NEXT:   1x unsupported: call to '<name>' declared in a system header; not part of the supported C subset
// CHECK-NEXT: rank 4. unsupported-stmt:GCCAsmStmt  items=1  tus=1
// CHECK-NEXT:   first: {{.*}}vol.c:{{[0-9]+}}:{{[0-9]+}}
// CHECK-NEXT:   1x unsupported statement: GCCAsmStmt
//
// The report requires shard artifacts: outside --link it is a clean error.
// RUN: not emitrust-cc --emit=rejection-report %s -o - 2>&1 | FileCheck %s --check-prefix=GATE
// GATE: error: --emit=rejection-report requires --link

int clean_add(int a, int b) { return a + b; }

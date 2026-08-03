// FR-58 slice 1, the two link-time diagnostics: a shard's deferred external
// whose symbol NO shard on the link line defines is the undefined-symbol
// link error ("unresolved external ... at link", located at the recorded
// declaration), and two shards defining the SAME module-level symbol with
// DIFFERENT shapes (here `struct Box`) is the shape-conflict link error.
// Both use --emit=rust so no cargo is needed; the shards are produced
// through the FR-56 shim exactly as a real build would.
//
// Undefined symbol: this file calls `add` but nothing else is on the link
// line, so the deferred obligation the shard carries cannot resolve.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
// RUN: not emitrust-cc --link %t.main.o --emit=rust -o %t.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=UNRESOLVED
// UNRESOLVED: link-merge-errors.c:{{[0-9]+}}:{{[0-9]+}}: error: unresolved external 'add' at link
//
// Shape conflict: companions A and B each define `struct Box`, with
// different fields; first occurrence wins only for IDENTICAL shapes, so
// this pair must be refused.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-merge-conflict-a.c -o %t.a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-merge-conflict-b.c -o %t.b.o
// RUN: not emitrust-cc --link %t.a.o %t.b.o --emit=rust -o %t.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CONFLICT
// CONFLICT: error: conflicting definitions of 'Box' at link: the shards disagree on its shape
//
// Shape equality is STRUCTURAL, not layout: companions with the same
// struct name and IDENTICAL layout (one int) but different FIELD NAMES
// must still conflict — field names are attributes of the definition
// (FR-58 slice 2, OperationEquivalence).
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-merge-fieldname-a.c -o %t.fa.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-merge-fieldname-b.c -o %t.fb.o
// RUN: not emitrust-cc --link %t.fa.o %t.fb.o --emit=rust -o %t.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FIELDNAME
// FIELDNAME: link-merge-fieldname-{{[ab]}}.c:{{[0-9]+}}:{{[0-9]+}}: error: conflicting definitions of 'S' at link: the shards disagree on its shape
//
// And the complement: two BYTE-IDENTICAL definitions of `struct S` dedup
// silently — the link succeeds and the merged output carries exactly one.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-merge-fieldname-a2.c -o %t.fa2.o
// RUN: emitrust-cc --link %t.fa.o %t.fa2.o --emit=rust -o %t.dedup.rs
// RUN: FileCheck %s --check-prefix=DEDUP < %t.dedup.rs
// DEDUP-COUNT-1: struct S
// DEDUP-NOT: struct S

int add(int a, int b);

int main(void) { return add(1, 2); }

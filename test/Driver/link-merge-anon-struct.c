// FR-151: the synthesized name of a bare anonymous struct is a deterministic
// function of its FIELD SHAPE, not of its first-encounter order within one
// import. The FR-58 shard path imports each translation unit in its own
// process, so an order-keyed name (the old per-import `Anon0`, `Anon1`, ...
// counter) made independent shards disagree: TU A's `Anon0` was one shape and
// TU B's `Anon0` another, and `mergeShards`, which dedups module-level defs
// by NAME under structural OperationEquivalence, refused the link with
// `conflicting definitions of 'Anon0' at link`. Measured on a real project:
// the 501-object `systemd-detect-virt` link failed on exactly this, at
// siginfo_t.h's anonymous member.
//
// This file pins the three properties the content-keyed name must hold at
// once, all of them through the shard path a real build takes:
//   (a) two shards with DIFFERENT anonymous shapes LINK, each shape keeping
//       its own struct (the FR-151 regression: this failed at HEAD);
//   (b) two shards reaching the SAME anonymous shape still dedup to EXACTLY
//       ONE struct, and every wrapper names it -- the property that rules out
//       the "tag the name per TU" fix, since the wrapper's field type embeds
//       the anonymous name and a per-TU tag would make the WRAPPER differ
//       between shards and turn a working link into a conflict;
//   (c) cross-invocation stability: one shape gets ONE name no matter what
//       else an import saw first (companion C imports an unrelated shape
//       ahead of it, which under the counter shifted it from `Anon0` to
//       `Anon1`).
//
// No test here may hard-code a hash literal: the name is derived from the
// shape key, and that key is free to change (FR-78's opaque-union arms and
// FR-122's ODR suffix already extend it) without churning goldens. Every
// anonymous name is captured into a FileCheck variable instead.

// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-merge-anon-b.c -o %t.b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-merge-anon-c.c -o %t.c.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-merge-anon-sa.c -o %t.sa.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-merge-anon-sb.c -o %t.sb.o

// (a) Different shapes in two shards: the link SUCCEEDS and both shapes
// survive as their own struct, each still wrapped by its own named record.
// RUN: emitrust-cc --link %t.a.o %t.b.o --emit=rust -o %t.ab.rs
// RUN: FileCheck %s --check-prefix=DIFFSHAPE < %t.ab.rs
// RUN: FileCheck %s --check-prefix=DIFFCOUNT < %t.ab.rs
// DIFFSHAPE: pub struct [[ANONA:Anon[0-9A-F]+]] {
// DIFFSHAPE-NEXT: pub a: i32,
// DIFFSHAPE: pub struct OuterA {
// DIFFSHAPE-NEXT: pub inner: [[ANONA]],
// DIFFSHAPE: pub struct [[ANONB:Anon[0-9A-F]+]] {
// DIFFSHAPE-NEXT: pub b: i64,
// DIFFSHAPE-NEXT: pub c: i64,
// DIFFSHAPE: pub struct OuterB {
// DIFFSHAPE-NEXT: pub inner: [[ANONB]],
// Exactly two anonymous structs: two shapes in, two out, nothing merged and
// nothing duplicated.
// DIFFCOUNT-COUNT-2: pub struct Anon
// DIFFCOUNT-NOT: pub struct Anon

// (b) One shape reached from two shards -- through a shared header AND
// through two independently written wrappers -- dedups to ONE struct that
// all three wrappers name.
// RUN: emitrust-cc --link %t.sa.o %t.sb.o --emit=rust -o %t.shared.rs
// RUN: FileCheck %s --check-prefix=SHARED < %t.shared.rs
// RUN: FileCheck %s --check-prefix=SHAREDCOUNT < %t.shared.rs
// SHARED: pub struct [[ANONS:Anon[0-9A-F]+]] {
// SHARED-NEXT: pub p: i32,
// SHARED-NEXT: pub q: i32,
// SHARED-DAG: pub pt: [[ANONS]],
// SHARED-DAG: pub struct SharedW {
// SHARED-DAG: pub struct WrapA {
// SHARED-DAG: pub struct WrapB {
// SHAREDCOUNT-COUNT-1: pub struct Anon
// SHAREDCOUNT-NOT: pub struct Anon
// Every wrapper's field is the ONE anonymous struct: three wrappers, three
// field lines, no second anonymous type.
// RUN: FileCheck %s --check-prefix=SHAREDFIELD < %t.shared.rs
// SHAREDFIELD: pub struct [[ANONF:Anon[0-9A-F]+]] {
// SHAREDFIELD-COUNT-3: pub pt: [[ANONF]],

// (c) Cross-invocation stability, seen from the shards THEMSELVES: the name
// companion C gives `{int a;}` -- which it imports SECOND, behind an
// unrelated `{short z;}` -- is the very name this file's shard gives it,
// captured from one shard's emitted crate and required of the other's.
// RUN: emitrust-cc --link %t.a.o --emit=rust -o %t.a.rs
// RUN: emitrust-cc --link %t.c.o --emit=rust -o %t.c.rs
// RUN: cat %t.a.rs %t.c.rs | FileCheck %s --check-prefix=STABLE
// STABLE: pub struct [[ANON:Anon[0-9A-F]+]] {
// STABLE-NEXT: pub a: i32,
// STABLE: pub struct OuterA {
// STABLE-NEXT: pub inner: [[ANON]],
// STABLE: pub struct HeadC {
// STABLE: pub struct [[ANON]] {
// STABLE-NEXT: pub a: i32,
// STABLE: pub struct OuterC {
// STABLE-NEXT: pub inner: [[ANON]],

// ... so the two shards LINK, with the shared shape deduped to one struct
// that both wrappers name and the unrelated shape kept apart.
// RUN: emitrust-cc --link %t.a.o %t.c.o --emit=rust -o %t.ac.rs
// RUN: FileCheck %s --check-prefix=SAMENAME < %t.ac.rs
// RUN: FileCheck %s --check-prefix=SAMECOUNT < %t.ac.rs
// SAMENAME: pub struct [[ANONAC:Anon[0-9A-F]+]] {
// SAMENAME-NEXT: pub a: i32,
// SAMENAME: pub struct OuterA {
// SAMENAME-NEXT: pub inner: [[ANONAC]],
// SAMENAME: pub struct OuterC {
// SAMENAME-NEXT: pub inner: [[ANONAC]],
// SAMECOUNT-COUNT-2: pub struct Anon
// SAMECOUNT-NOT: pub struct Anon

struct OuterA {
  struct {
    int a;
  } inner;
};

int anon_a(int v) {
  struct OuterA o;
  o.inner.a = v;
  return o.inner.a;
}

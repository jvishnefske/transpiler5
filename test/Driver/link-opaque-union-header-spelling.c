// FR-173 D1: FR-78's cross-TU opaque-union shape key must be TU-STABLE.
//
// An anonymous union whose arms are all aggregates imports under FR-78 as
// one opaque byte blob, and every such blob has the SAME field shape
// (`["opaque"], [u8;N]`). The C arm types are therefore folded into the
// shape key that names it `Anon<hash>`, so that two structurally different
// opaque unions never merge -- while the same union reached through a
// shared header from several translation units still dedups to one
// struct_def.
//
// The fold spelled the arm type with clang's DEFAULT printing policy. For
// a TAGLESS arm -- `struct { int a; int b; } value;`, systemd's
// `BusMatchNode` shape -- that policy prints `(unnamed at <PATH>:L:C)`,
// and <PATH> is whatever THAT TU's SourceManager recorded: the same header
// reached through two spellings of one directory gives two different keys,
// two different `Anon<hash>` names, and therefore two structurally
// different wrappers. Measured at HEAD on this file, the arm structs
// themselves hashed identically (FR-151 content-hashes those) while the
// union hashed `Anon0184A871A2F2` under `-I %t/inc` and `Anon20FFE173CFEA`
// under `-I %t/inc/../inc`, and the link died with
// `conflicting definitions of 'Node' at link: the shards disagree on its
// shape` -- a whole-crate loss caused by nothing but an include-path
// spelling. In systemd the same divergence instead demoted the wrapper
// into a per-TU module, which is a silent correctness hazard rather than a
// loud one.
//
// The key is now printed with anonymous tag LOCATIONS suppressed and the
// arm's own size and ODR hash folded in instead: no source path can reach
// it, and two genuinely different tagless arms still separate (the
// `anon-distinct.c` leg of test/Import/C/union-opaque-aggregate.c pins
// that side).
//
// RUN: split-file %s %t

// The importer half, and the sharpest form of the invariant: the SAME
// header, reached through two spellings of one directory, must name the
// blob identically in both TUs. Both modules go into one stream so the
// FileCheck capture has to match across them.
// RUN: emitrust-import-c -I %t/inc %t/a.c 2>/dev/null > %t/both.mlir
// RUN: emitrust-import-c -I %t/inc/../inc %t/b.c 2>/dev/null >> %t/both.mlir
// RUN: FileCheck %s --check-prefix=SAME < %t/both.mlir
//      SAME: emitrust.struct_def @[[UNION:Anon[0-9A-F]+]] ["opaque"] [!emitrust.array<8xui8>] {emitrust.opaque_union}
//      SAME: emitrust.struct_def @Node ["type_", "u"] [i32, !emitrust.struct<"[[UNION]]">]
//      SAME: emitrust.struct_def @[[UNION]] ["opaque"] [!emitrust.array<8xui8>] {emitrust.opaque_union}
//      SAME: emitrust.struct_def @Node ["type_", "u"] [i32, !emitrust.struct<"[[UNION]]">]

// The link half: the shards agree, so `Node` and the blob each dedup to
// ONE crate-level type and nothing is demoted into a per-TU module.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/a.c -I %t/inc -o %t/a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/b.c -I %t/inc/../inc -o %t/b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/main.c -I %t/inc -o %t/main.o
// RUN: emitrust-cc --link %t/a.o %t/b.o %t/main.o --emit=rust -o %t.rs
// RUN: FileCheck %s --check-prefix=NODE --implicit-check-not="mod tu" < %t.rs
// NODE-COUNT-1: struct Node {
//     NODE-NOT: struct Node {
// RUN: FileCheck %s --check-prefix=BLOB < %t.rs
// BLOB-COUNT-1: opaque: [u8; 8],
//     BLOB-NOT: opaque: [u8; 8],

//--- inc/h.h
// Both arms are TAGLESS aggregates of the same size, which is exactly the
// FR-78 opaque-blob admission and exactly the shape whose canonical type
// string carried a source path.
struct Node {
  int type;
  union {
    struct {
      int a;
      int b;
    } value;
    struct {
      long c;
    } leaf;
  } u;
};
int use_a(struct Node *n);
int use_b(struct Node *n);

//--- a.c
#include "h.h"
int use_a(struct Node *n) { return n->type + 1; }

//--- b.c
#include "h.h"
int use_b(struct Node *n) { return n->type + 2; }

//--- main.c
#include "h.h"
int printf(const char *, ...);
int main(int argc, char **argv) {
  struct Node n;
  n.type = argc;
  printf("%d %d\n", use_a(&n), use_b(&n));
  return 0;
}

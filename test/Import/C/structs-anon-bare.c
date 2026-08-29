// CTS-R1: bare anonymous struct types (no tag, no typedef name) import
// under a synthesized `Anon<hash>` name keyed by field shape: two distinct
// shapes get distinct names, the same shape twice shares one struct_def,
// and — because C type identity is by declaration, not shape — an anonymous
// struct whose shape matches a NAMED struct's still gets its own Rust type.
// FR-151: the name is the CONTENT HASH of the shape key (uppercase hex of
// `xxh3_64bits`), not a first-encounter counter, so the same anonymous shape
// imports to the same Rust type in every translation unit AND in every
// separate `emitrust-clang -c` invocation of the FR-58 shard path — the
// cross-invocation half is pinned in test/Driver/link-merge-anon-struct.c.
// No hash literal is spelled out here: every anonymous name is captured into
// a FileCheck variable, so extending the shape key (FR-78's opaque-union
// arms, FR-122's ODR suffix) never churns this golden.
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/shapes.c > %t/shapes.mlir
// RUN: FileCheck %s < %t/shapes.mlir
// RUN: FileCheck %s --check-prefix=COUNT < %t/shapes.mlir
// RUN: emitrust-import-c %t/named-vs-anon.c | FileCheck %s --check-prefix=NAMED
// RUN: emitrust-import-c %t/tu-a.c %t/tu-b.c | FileCheck %s --check-prefix=DEDUP
// RUN: emitrust-import-c %t/anon-enum-member.c | FileCheck %s --check-prefix=ENUM

//--- shapes.c
// A file-scope anonymous struct with an initializer, an anonymous struct
// member nested inside a named struct, and a local whose shape repeats.
struct { int a; int b; int c; } g = {1, 2, 3};

struct outer {
  int x;
  struct {
    int y;
    int z;
  } nest;
};

int consume(void) {
  struct { int a; int b; int c; } local; // same shape as `g`: shares its name
  struct { int only; } other;           // distinct shape: gets its own name
  struct outer v;
  local.a = g.a;
  local.b = g.b;
  local.c = g.c;
  other.only = 4;
  v.x = 5;
  v.nest.y = 6;
  v.nest.z = 7;
  return local.a + local.b + local.c + other.only + v.x + v.nest.y + v.nest.z;
}

// Emission order still follows first encounter — the global's shape, the
// nested member's shape, then the one new local shape (whose struct_def is
// appended after the already-created function) — but the NAMES no longer do:
// `local`, whose shape repeats `g`'s, resolves to `g`'s name.
// CHECK: emitrust.struct_def @[[ABC:Anon[0-9A-F]+]] ["a", "b", "c"] [i32, i32, i32]
// CHECK: emitrust.global @g <[1 : i32, 2 : i32, 3 : i32]> : !emitrust.struct<"[[ABC]]">
// CHECK: emitrust.struct_def @[[YZ:Anon[0-9A-F]+]] ["y", "z"] [i32, i32]
// CHECK: emitrust.struct_def @outer ["x", "nest"] [i32, !emitrust.struct<"[[YZ]]">]
// CHECK-LABEL: func.func @consume
// CHECK: emitrust.variable named "local" : !emitrust.lvalue<!emitrust.struct<"[[ABC]]">>
// CHECK: emitrust.variable named "other" : !emitrust.lvalue<!emitrust.struct<"[[ONLY:Anon[0-9A-F]+]]">>
// CHECK: emitrust.struct_def @[[ONLY]] ["only"] [i32]
// Exactly three anonymous shapes exist; no fourth name is minted (the
// repeated local shape reuses the global's).
// COUNT-COUNT-3: emitrust.struct_def @Anon
// COUNT-NOT: emitrust.struct_def @Anon

//--- named-vs-anon.c
// The anonymous struct's shape equals `struct pt`'s, but C type identity is
// by declaration: the anonymous type keeps its own struct_def and name.
struct pt { int x; int y; };

struct pt p;
struct { int x; int y; } q;

// NAMED: emitrust.struct_def @pt ["x", "y"] [i32, i32]
// NAMED: emitrust.global @p : !emitrust.struct<"pt">
// NAMED: emitrust.struct_def @[[XY:Anon[0-9A-F]+]] ["x", "y"] [i32, i32]
// NAMED: emitrust.global @q : !emitrust.struct<"[[XY]]">

//--- tu-a.c
struct { int lo; int hi; } range_a;

//--- tu-b.c
struct { int lo; int hi; } range_b;

// The identical anonymous shape in two TUs maps to one synthesized name and
// one struct_def.
// DEDUP: emitrust.struct_def @[[LOHI:Anon[0-9A-F]+]] ["lo", "hi"] [i32, i32]
// DEDUP-NOT: emitrust.struct_def @Anon
// DEDUP-DAG: emitrust.global @range_a : !emitrust.struct<"[[LOHI]]">
// DEDUP-DAG: emitrust.global @range_b : !emitrust.struct<"[[LOHI]]">

//--- anon-enum-member.c
// An anonymous-enum-typed member is a plain `i32` field (anonymous enums are
// plain `int` values everywhere; their enumerators import as i32 constants).
struct {
  enum { X } x;
} s;

int read_x(void) { return X; }

// ENUM: emitrust.struct_def @[[EX:Anon[0-9A-F]+]] ["x"] [i32]
// ENUM: emitrust.global @s : !emitrust.struct<"[[EX]]">
// ENUM-LABEL: func.func @read_x
// ENUM: arith.constant 0 : i32

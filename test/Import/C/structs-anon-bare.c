// CTS-R1: bare anonymous struct types (no tag, no typedef name) import
// under a synthesized `Anon<n>` name keyed by field shape: two distinct
// shapes get distinct names, the same shape twice shares one struct_def,
// and — because C type identity is by declaration, not shape — an anonymous
// struct whose shape matches a NAMED struct's still gets its own Rust type.
// The name is a deterministic function of the shape, so the same anonymous
// shape in two translation units imports exactly once.
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/shapes.c | FileCheck %s
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
  struct { int a; int b; int c; } local; // same shape as `g`: shares Anon0
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

// First-encounter order: the global's shape, the nested member's shape,
// then the one new local shape (whose struct_def is appended after the
// already-created function); the repeated local shape reuses Anon0.
// CHECK-DAG: emitrust.struct_def @Anon0 ["a", "b", "c"] [i32, i32, i32]
// CHECK-DAG: emitrust.struct_def @Anon1 ["y", "z"] [i32, i32]
// CHECK-DAG: emitrust.struct_def @outer ["x", "nest"] [i32, !emitrust.struct<"Anon1">]
// CHECK-DAG: emitrust.global @g <[1 : i32, 2 : i32, 3 : i32]> : !emitrust.struct<"Anon0">
// CHECK-LABEL: func.func @consume
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Anon0">>
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Anon2">>
// CHECK: emitrust.struct_def @Anon2 ["only"] [i32]
// Exactly three anonymous shapes exist; no fourth name is minted.
// CHECK-NOT: @Anon3

//--- named-vs-anon.c
// The anonymous struct's shape equals `struct pt`'s, but C type identity is
// by declaration: the anonymous type keeps its own struct_def and name.
struct pt { int x; int y; };

struct pt p;
struct { int x; int y; } q;

// NAMED-DAG: emitrust.struct_def @pt ["x", "y"] [i32, i32]
// NAMED-DAG: emitrust.struct_def @Anon0 ["x", "y"] [i32, i32]
// NAMED-DAG: emitrust.global @p : !emitrust.struct<"pt">
// NAMED-DAG: emitrust.global @q : !emitrust.struct<"Anon0">

//--- tu-a.c
struct { int lo; int hi; } range_a;

//--- tu-b.c
struct { int lo; int hi; } range_b;

// The identical anonymous shape in two TUs maps to one synthesized name and
// one struct_def.
// DEDUP: emitrust.struct_def @Anon0 ["lo", "hi"] [i32, i32]
// DEDUP-NOT: emitrust.struct_def @Anon
// DEDUP-DAG: emitrust.global @range_a : !emitrust.struct<"Anon0">
// DEDUP-DAG: emitrust.global @range_b : !emitrust.struct<"Anon0">

//--- anon-enum-member.c
// An anonymous-enum-typed member is a plain `i32` field (anonymous enums are
// plain `int` values everywhere; their enumerators import as i32 constants).
struct {
  enum { X } x;
} s;

int read_x(void) { return X; }

// ENUM: emitrust.struct_def @Anon0 ["x"] [i32]
// ENUM: emitrust.global @s : !emitrust.struct<"Anon0">
// ENUM-LABEL: func.func @read_x
// ENUM: arith.constant 0 : i32

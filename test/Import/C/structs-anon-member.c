// CTS-R2: anonymous struct/union MEMBERS (C11 6.7.2.1p13 — an unnamed
// member whose type is an anonymous record) flatten into the parent
// struct_def: an anonymous struct member's fields join the parent's
// member namespace under their own spellings (nested members flatten
// recursively), and an anonymous union member whose arms all flatten to
// one leaf of one identical type becomes a single storage slot that
// every arm's spelling aliases. Member access through the parent skips
// the implicit intermediate access Sema synthesizes, and initializer
// lists (block-scope assigns and constant global attributes alike)
// land on the flattened fields. Distinct from CTS-R1's bare anonymous
// struct declarations, which keep their own `Anon<n>` Rust types.
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/flatten.c | FileCheck %s
// RUN: emitrust-import-c %t/union-slot.c | FileCheck %s --check-prefix=SLOT
// RUN: emitrust-import-c %t/init.c | FileCheck %s --check-prefix=INIT

//--- flatten.c
// Anonymous struct members nested two levels deep: every leaf joins
// `outer`'s namespace, so the struct_def has one flat field list and
// each access selects its leaf directly on the parent place.
struct outer {
  int a;
  struct {
    int b;
    struct {
      int c;
    };
  };
  int d;
};

int touch(void) {
  struct outer v;
  v.a = 1;
  v.b = 2;
  v.c = 3;
  v.d = 4;
  return v.a + v.b + v.c + v.d;
}

// CHECK: emitrust.struct_def @outer ["a", "b", "c", "d"] [i32, i32, i32, i32]
// CHECK-LABEL: func.func @touch
// CHECK: emitrust.member %{{.*}}["b"] : (!emitrust.lvalue<!emitrust.struct<"outer">>) -> !emitrust.lvalue<i32>
// CHECK: emitrust.member %{{.*}}["c"] : (!emitrust.lvalue<!emitrust.struct<"outer">>) -> !emitrust.lvalue<i32>

//--- union-slot.c
// An anonymous union member whose arms are one identical type is one
// storage slot named after the first arm; reading any other arm reads
// that slot (same type as the last store, so the value is exact). The
// nested struct-in-union-in-struct chain also collapses to its single
// leaf. No `b2` member ever reaches the IR.
typedef struct {
  int a;
  union {
    int b1;
    int b2;
  };
  struct { union { struct { int c; }; }; };
} s;

int alias(void) {
  s v;
  v.a = 1;
  v.b1 = 2;
  v.c = 3;
  return v.a + v.b2 + v.c;
}

// SLOT: emitrust.struct_def @s ["a", "b1", "c"] [i32, i32, i32]
// SLOT-LABEL: func.func @alias
// SLOT: emitrust.member %{{.*}}["b1"]
// SLOT: emitrust.member %{{.*}}["c"]
// SLOT-NOT: ["b2"]

//--- init.c
// Initializer shapes: the global's constant initializer flattens the
// anonymous union's nested list onto its slot (brace-elided values stay
// positional), a partially initialized global zero-fills the flattened
// tail, and a block-scope list assigns through the flattened members.
struct S1 { int a; int b; };

struct S2 {
  int a;
  int b;
  union {
    int c;
    int d;
  };
  struct S1 s;
};

struct S2 g = {1, 2, 3, {4, 5}};
struct S2 h = {6};

int locals(void) {
  struct S2 v = {6, 7, 8, {9, 10}};
  return v.a + v.c + v.s.b + g.d + h.c;
}

// INIT-DAG: emitrust.struct_def @S1 ["a", "b"] [i32, i32]
// INIT-DAG: emitrust.struct_def @S2 ["a", "b", "c", "s"] [i32, i32, i32, !emitrust.struct<"S1">]
// INIT-DAG: emitrust.global @g <[1 : i32, 2 : i32, 3 : i32, [4 : i32, 5 : i32]]> : !emitrust.struct<"S2">
// INIT-DAG: emitrust.global @h <[6 : i32, 0 : i32, 0 : i32, [0 : i32, 0 : i32]]> : !emitrust.struct<"S2">
// INIT-LABEL: func.func @locals
// INIT: emitrust.member %{{.*}}["c"] : (!emitrust.lvalue<!emitrust.struct<"S2">>) -> !emitrust.lvalue<i32>
// INIT-NOT: ["d"]

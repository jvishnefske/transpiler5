// FR-78: a union whose arms are ALL aggregates of differing mapped types
// (lwIP's `union { ip6_addr_t; ip4_addr_t; }` shape) has no one-slot model
// — exactly where the C99-44 residual rejection ("union arm cannot alias
// the storage slot") used to kill the union AND cascade through every
// record naming it, the union now imports as OPAQUE STORAGE: a one-field
// struct_def whose single field is a sizeof-sized byte blob, marked
// `emitrust.opaque_union`. This file pins the CONTAINMENT win (the
// invariant this FR exists for): records containing such a union import,
// whole-value traffic (record copies, union by-value args/returns,
// member-to-member union copies, all-zero constant globals) works, and no
// arm name ever reaches the IR. Every access THROUGH an arm stays a
// located rejection at its own site (union-opaque-aggregate-invalid.c),
// and the emitter refuses any leaked arm access (errors.mlir) — the
// pre-FR-78 placeholder attempt died as rustc E0609 precisely because its
// guard was not access-complete. The one-slot matrix (unions.c,
// unions-invalid.c, union-bytearray-arm*.c) is untouched: the opaque model
// triggers only where the residual rejection would have fired and only
// when every arm is an aggregate.
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/contained.c | FileCheck %s --check-prefix=CONTAINED
// RUN: emitrust-import-c %t/wholevalue.c | FileCheck %s --check-prefix=WHOLE
// RUN: emitrust-import-c %t/anon-distinct.c | FileCheck %s --check-prefix=ANON
// RUN: emitrust-import-c %t/global-zero.c | FileCheck %s --check-prefix=GLOBALZERO

//--- contained.c
// The containment shape: the record around the opaque union imports, its
// sibling members are fully usable (read through a pointer param, written
// by value, returned by value, whole-record copied), and neither arm
// spelling appears anywhere in the IR.
struct A {
  int ax[10];
};

struct B {
  short bs;
  char bc;
};

union U {
  struct A a;
  struct B b;
};

struct Rec {
  int before;
  union U u;
  int after;
};

int sum_rec(struct Rec *r) { return r->before + r->after; }

struct Rec make_rec(int seed) {
  struct Rec r;
  r.before = seed;
  r.after = seed * 2 + 1;
  return r;
}

int take_by_value(struct Rec r) { return r.after - r.before; }

int copy_rec(void) {
  struct Rec r = make_rec(5);
  struct Rec s = r;
  return take_by_value(s);
}

// CONTAINED-DAG: emitrust.struct_def @[[U:([A-Za-z0-9_]+_)?U]] ["opaque"] [!emitrust.array<40xui8>] {emitrust.opaque_union}
// CONTAINED-DAG: emitrust.struct_def @[[R:([A-Za-z0-9_]+_)?Rec]] ["before", "u", "after"] [i32, !emitrust.struct<"[[U]]">, i32]
// CONTAINED-LABEL: func.func @sum_rec
// CONTAINED-LABEL: func.func @make_rec
// CONTAINED-LABEL: func.func @take_by_value
// CONTAINED-LABEL: func.func @copy_rec
// CONTAINED-NOT: ["a"]
// CONTAINED-NOT: ["b"]

//--- wholevalue.c
// Whole-VALUE traffic never opens the blob: the union passes and returns
// by value, and copies member-to-member between records; the sole member
// selection on a Rec is the union FIELD itself (an lvalue of the opaque
// struct type), never an arm.
struct A {
  int ax[10];
};

struct B {
  short bs;
  char bc;
};

union U {
  struct A a;
  struct B b;
};

struct Rec {
  int before;
  union U u;
  int after;
};

union U pass(union U v) { return v; }

int whole(void) {
  union U tmp;
  union U back;
  struct Rec r;
  r.before = 3;
  r.after = 4;
  r.u = tmp;
  back = r.u;
  back = pass(back);
  struct Rec s = r;
  return s.before + s.after;
}

// WHOLE-DAG: emitrust.struct_def @[[U:([A-Za-z0-9_]+_)?U]] ["opaque"] [!emitrust.array<40xui8>] {emitrust.opaque_union}
// WHOLE: func.func @pass(%{{[a-z0-9]+}}: !emitrust.struct<"[[U]]">) -> !emitrust.struct<"[[U]]">
// WHOLE-LABEL: func.func @whole
// WHOLE: emitrust.member %{{.*}}["u"] : (!emitrust.lvalue<!emitrust.struct<"{{([A-Za-z0-9_]+_)?}}Rec">>) -> !emitrust.lvalue<!emitrust.struct<"[[U]]">>
// WHOLE-NOT: ["a"]
// WHOLE-NOT: ["b"]

//--- anon-distinct.c
// Two structurally DIFFERENT anonymous-type opaque unions with the SAME
// blob size must not merge under the shape-keyed `Anon<hash>` naming: the
// blob field shape is identical (["opaque"], [u8;8]), so the C arm types
// are folded into the shape key. Two struct_defs carrying the marker pin
// the no-merge invariant (each CHECK-DAG must match a distinct line).
struct C1 {
  int ca;
  int cb;
};

struct D1 {
  short ds;
};

struct E1 {
  double ed;
};

struct F1 {
  int fi;
};

struct RecA {
  union {
    struct C1 c;
    struct D1 d;
  } u;
};

struct RecB {
  union {
    struct E1 e;
    struct F1 f;
  } u;
};

// FR-151 widened the synthesized spelling from a counter to uppercase hex
// of the shape hash; the property pinned here is unchanged -- TWO DISTINCT
// anonymous opaque unions (two DAG matches of the same pattern cannot land
// on one line), one per record.
// ANON-DAG: emitrust.struct_def @{{Anon[0-9A-F]+}} ["opaque"] [!emitrust.array<8xui8>] {emitrust.opaque_union}
// ANON-DAG: emitrust.struct_def @{{Anon[0-9A-F]+}} ["opaque"] [!emitrust.array<8xui8>] {emitrust.opaque_union}
// ANON-DAG: emitrust.struct_def @{{([A-Za-z0-9_]+_)?}}RecA ["u"] [!emitrust.struct<"Anon{{[0-9A-F]+}}">]
// ANON-DAG: emitrust.struct_def @{{([A-Za-z0-9_]+_)?}}RecB ["u"] [!emitrust.struct<"Anon{{[0-9A-F]+}}">]

//--- global-zero.c
// Constant globals: an ALL-ZERO union constant — whether the braces name
// an active arm or the union arrives zero-filled inside a record — is
// exactly static-storage zero-fill and lands on the blob's zero value; a
// NON-zero arm constant stays rejected at the initializer (see
// union-opaque-aggregate-invalid.c).
struct A {
  int ax[10];
};

struct B {
  short bs;
  char bc;
};

union U {
  struct A a;
  struct B b;
};

struct Rec {
  int before;
  union U u;
  int after;
};

union U gz = {0};
struct Rec gr = {7};

int read_globals(void) { return gr.before + gr.after; }

// GLOBALZERO-DAG: emitrust.struct_def @[[U:([A-Za-z0-9_]+_)?U]] ["opaque"] [!emitrust.array<40xui8>] {emitrust.opaque_union}
// GLOBALZERO-DAG: emitrust.global @gz <{{.*}}> : !emitrust.struct<"[[U]]">
// GLOBALZERO-DAG: emitrust.global @gr <[7 : i32, {{.*}}, 0 : i32]> : !emitrust.struct<"{{([A-Za-z0-9_]+_)?}}Rec">
// GLOBALZERO-LABEL: func.func @read_globals

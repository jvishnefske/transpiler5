// B1: named (and untagged) union RecordDecls import as a ONE-FIELD struct
// whose single storage field is the FIRST arm's leaf type, generalizing the
// anonymous-union slot machinery (CTS-R2): every arm's spelling aliases
// that one slot, so no non-first arm name ever reaches the IR. In scope:
// arms that all flatten to one identical mapped type, same-width int arms
// differing only in signedness — accesses through the differently-signed
// arm wrap an `emitrust.cast` (bit-exact on two's complement) — a float
// arm paired with a same-width integer (float/32-bit, double/64-bit) —
// accesses through the cross-domain arm wrap an `emitrust.bitcast`
// (`to_bits`/`from_bits`, bit-exact by definition) — and single-arm
// unions. A union global's constant initializer lands on the slot like
// any one-field struct's; a float-pun arm's constant crosses the domain
// at compile time as its exact bit pattern. A designated local
// initializer through a pun arm reinterprets onto the slot the same way.
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/alias.c | FileCheck %s --check-prefix=ALIAS
// RUN: emitrust-import-c %t/single.c | FileCheck %s --check-prefix=SINGLE
// RUN: emitrust-import-c %t/member.c | FileCheck %s --check-prefix=MEMBER
// RUN: emitrust-import-c %t/global.c | FileCheck %s --check-prefix=GLOBAL
// RUN: emitrust-import-c %t/pun.c | FileCheck %s --check-prefix=PUN
// RUN: emitrust-import-c %t/untagged.c | FileCheck %s --check-prefix=UNTAGGED
// RUN: emitrust-import-c %t/floatpun.c | FileCheck %s --check-prefix=FLOATPUN
// RUN: emitrust-import-c %t/floatslot.c | FileCheck %s --check-prefix=FLOATSLOT
// RUN: emitrust-import-c %t/doublepun.c | FileCheck %s --check-prefix=DOUBLEPUN
// RUN: emitrust-import-c %t/puninit.c | FileCheck %s --check-prefix=PUNINIT

//--- alias.c
// Two arms of one identical type: one slot named after the first arm;
// writing `a` and reading `b` both select that slot.
union Both {
  int a;
  int b;
};

int write_read(void) {
  union Both v;
  v.a = 41;
  v.b = v.b + 1;
  return v.b;
}

// ALIAS-DAG: emitrust.struct_def @[[U:([A-Za-z0-9_]+_)?Both]] ["a"] [i32]
// ALIAS-LABEL: func.func @write_read
// ALIAS: emitrust.variable named "v" : !emitrust.lvalue<!emitrust.struct<"[[U]]">>
// ALIAS: emitrust.member %{{.*}}["a"] : (!emitrust.lvalue<!emitrust.struct<"[[U]]">>) -> !emitrust.lvalue<i32>
// ALIAS-NOT: ["b"]

//--- single.c
// A single-arm union is exactly its one-field struct.
union One {
  int only;
};

int single(void) {
  union One s;
  s.only = 7;
  return s.only;
}

// SINGLE-DAG: emitrust.struct_def @[[U:([A-Za-z0-9_]+_)?One]] ["only"] [i32]
// SINGLE-LABEL: func.func @single
// SINGLE: emitrust.member %{{.*}}["only"] : (!emitrust.lvalue<!emitrust.struct<"[[U]]">>) -> !emitrust.lvalue<i32>

//--- member.c
// A named union used as a struct member: the member's type is the union's
// one-field struct, and the chained access h.v.b selects the slot `a`.
union Val {
  int a;
  int b;
};

struct Holder {
  int tag;
  union Val v;
};

int through(void) {
  struct Holder h;
  h.tag = 1;
  h.v.a = 2;
  return h.tag + h.v.b;
}

// MEMBER-DAG: emitrust.struct_def @[[U:([A-Za-z0-9_]+_)?Val]] ["a"] [i32]
// MEMBER-DAG: emitrust.struct_def @[[H:([A-Za-z0-9_]+_)?Holder]] ["tag", "v"] [i32, !emitrust.struct<"[[U]]">]
// MEMBER-LABEL: func.func @through
// MEMBER: emitrust.member %{{.*}}["v"] : (!emitrust.lvalue<!emitrust.struct<"[[H]]">>) -> !emitrust.lvalue<!emitrust.struct<"[[U]]">>
// MEMBER: emitrust.member %{{.*}}["a"] : (!emitrust.lvalue<!emitrust.struct<"[[U]]">>) -> !emitrust.lvalue<i32>
// MEMBER-NOT: ["b"]

//--- global.c
// A file-scope union with a constant initializer: the initializer lands on
// the single slot exactly like a one-field struct's initializer list.
union Both {
  int a;
  int b;
};

union Both g = {5};

int read_global(void) { return g.b; }

// GLOBAL-DAG: emitrust.struct_def @[[U:([A-Za-z0-9_]+_)?Both]] ["a"] [i32]
// GLOBAL-DAG: emitrust.global @g <[5 : i32]> : !emitrust.struct<"[[U]]">
// GLOBAL-LABEL: func.func @read_global
// GLOBAL: emitrust.member %{{.*}}["a"] : (!emitrust.lvalue<!emitrust.struct<"[[U]]">>) -> !emitrust.lvalue<i32>
// GLOBAL-NOT: ["b"]

//--- pun.c
// Same-width int arms differing only in signedness: the slot takes the
// FIRST arm's type (i32); every access through the differently-signed arm
// still selects that slot and wraps an emitrust.cast — reads cast the
// loaded i32 to ui32, stores cast the ui32 value to i32 before landing on
// the slot. Bit-exact both ways (two's complement), matching C99 6.5.2.3
// union punning. The `u` spelling itself never reaches the IR.
union Pun {
  int i;
  unsigned int u;
};

unsigned int pun_read(void) {
  union Pun p;
  p.i = -1;
  return p.u;
}

int pun_write(void) {
  union Pun p;
  p.u = 4294967295u;
  return p.i;
}

// PUN-DAG: emitrust.struct_def @[[U:([A-Za-z0-9_]+_)?Pun]] ["i"] [i32]
// PUN-LABEL: func.func @pun_read
// PUN: emitrust.member %{{.*}}["i"] : (!emitrust.lvalue<!emitrust.struct<"[[U]]">>) -> !emitrust.lvalue<i32>
// PUN: emitrust.cast %{{.*}} : i32 to ui32
// PUN-LABEL: func.func @pun_write
// PUN: emitrust.cast %{{.*}} : ui32 to i32
// PUN: emitrust.member %{{.*}}["i"] : (!emitrust.lvalue<!emitrust.struct<"[[U]]">>) -> !emitrust.lvalue<i32>
// PUN-NOT: ["u"]

//--- untagged.c
// An untagged local union (the c-testsuite 00042.c shape) imports under a
// synthesized name, same one-slot model: both spellings alias the slot.
int untagged(void) {
  union {
    int a;
    int b;
  } u;
  u.a = 1;
  u.b = 3;
  if (u.a != 3 || u.b != 3)
    return 1;
  return 0;
}

// UNTAGGED-LABEL: func.func @untagged
// UNTAGGED: emitrust.variable named "u" : !emitrust.lvalue<!emitrust.struct<"[[U:[A-Za-z_][A-Za-z0-9_]*]]">>
// UNTAGGED: emitrust.member %{{.*}}["a"] : (!emitrust.lvalue<!emitrust.struct<"[[U]]">>) -> !emitrust.lvalue<i32>
// UNTAGGED-NOT: ["b"]

//--- floatpun.c
// The float pun over an integer slot (the classic type-punning shape):
// the slot takes the FIRST arm's type (ui32); reads through the float arm
// bitcast the loaded slot value to f32, stores bitcast the f32 value back
// to ui32 — Rust's from_bits/to_bits, bit-exact in both directions.
union FPun {
  unsigned int u;
  float f;
};

float fpun_read(void) {
  union FPun p;
  p.u = 0x40200000u;
  return p.f;
}

unsigned int fpun_write(float x) {
  union FPun p;
  p.f = x;
  return p.u;
}

// FLOATPUN-DAG: emitrust.struct_def @[[U:([A-Za-z0-9_]+_)?FPun]] ["u"] [ui32]
// FLOATPUN-LABEL: func.func @fpun_read
// FLOATPUN: emitrust.member %{{.*}}["u"] : (!emitrust.lvalue<!emitrust.struct<"[[U]]">>) -> !emitrust.lvalue<ui32>
// FLOATPUN: emitrust.bitcast %{{.*}} : ui32 to f32
// FLOATPUN-LABEL: func.func @fpun_write
// FLOATPUN: emitrust.bitcast %{{.*}} : f32 to ui32
// FLOATPUN-NOT: ["f"]

//--- floatslot.c
// Declaration order decides the slot: a FIRST float arm makes the slot
// f32, and accesses through the integer arm carry the bitcasts. A global
// initializer designating the integer arm lands on the float slot as its
// exact bit pattern at compile time (0x3fc00000 is 1.5f).
union FSlot {
  float f;
  unsigned int u;
};

union FSlot g = {.u = 0x3fc00000u};

unsigned int fslot_bits(void) {
  union FSlot p;
  p.f = 1.5f;
  return p.u;
}

// FLOATSLOT-DAG: emitrust.struct_def @[[U:([A-Za-z0-9_]+_)?FSlot]] ["f"] [f32]
// FLOATSLOT-DAG: emitrust.global @g <[1.500000e+00 : f32]> : !emitrust.struct<"[[U]]">
// FLOATSLOT-LABEL: func.func @fslot_bits
// FLOATSLOT: emitrust.member %{{.*}}["f"] : (!emitrust.lvalue<!emitrust.struct<"[[U]]">>) -> !emitrust.lvalue<f32>
// FLOATSLOT: emitrust.bitcast %{{.*}} : f32 to ui32
// FLOATSLOT-NOT: ["u"]

//--- doublepun.c
// The 64-bit pair: double aliases a same-width integer arm (long, i64)
// the same way, through f64 bitcasts.
union DPun {
  double d;
  long b;
};

long dpun_bits(double x) {
  union DPun p;
  p.d = x;
  return p.b;
}

// DOUBLEPUN-DAG: emitrust.struct_def @[[U:([A-Za-z0-9_]+_)?DPun]] ["d"] [f64]
// DOUBLEPUN-LABEL: func.func @dpun_bits
// DOUBLEPUN: emitrust.member %{{.*}}["d"] : (!emitrust.lvalue<!emitrust.struct<"[[U]]">>) -> !emitrust.lvalue<f64>
// DOUBLEPUN: emitrust.bitcast %{{.*}} : f64 to i64
// DOUBLEPUN-NOT: ["b"]

//--- puninit.c
// A designated LOCAL initializer through a pun arm: the member place
// takes the SLOT's type and the arm-typed value reinterprets onto it —
// the signedness pun through a same-width cast, the float pun through a
// bitcast. (Regression: this used to store the arm-typed value into the
// slot-typed field, which produced uncompilable Rust.)
union Pun {
  int i;
  unsigned int u;
};

union FPun {
  unsigned int u;
  float f;
};

int pun_init(void) {
  union Pun p = {.u = 4294967295u};
  union FPun q = {.f = 1.5f};
  return p.i + (int)q.u;
}

// PUNINIT-LABEL: func.func @pun_init
// PUNINIT: emitrust.member %{{.*}}["i"] : (!emitrust.lvalue<!emitrust.struct<"{{([A-Za-z0-9_]+_)?}}Pun">>) -> !emitrust.lvalue<i32>
// PUNINIT: emitrust.cast %{{.*}} : ui32 to i32
// PUNINIT: emitrust.member %{{.*}}["u"] : (!emitrust.lvalue<!emitrust.struct<"{{([A-Za-z0-9_]+_)?}}FPun">>) -> !emitrust.lvalue<ui32>
// PUNINIT: emitrust.bitcast %{{.*}} : f32 to ui32

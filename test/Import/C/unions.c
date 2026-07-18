// B1: named (and untagged) union RecordDecls import as a ONE-FIELD struct
// whose single storage field is the FIRST arm's leaf type, generalizing the
// anonymous-union slot machinery (CTS-R2): every arm's spelling aliases
// that one slot, so no non-first arm name ever reaches the IR. In scope:
// arms that all flatten to one identical mapped type, same-width int arms
// differing only in signedness — accesses through the differently-signed
// arm wrap an `emitrust.cast` (bit-exact on two's complement) — and
// single-arm unions. A union global's constant initializer lands on the
// slot like any one-field struct's.
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/alias.c | FileCheck %s --check-prefix=ALIAS
// RUN: emitrust-import-c %t/single.c | FileCheck %s --check-prefix=SINGLE
// RUN: emitrust-import-c %t/member.c | FileCheck %s --check-prefix=MEMBER
// RUN: emitrust-import-c %t/global.c | FileCheck %s --check-prefix=GLOBAL
// RUN: emitrust-import-c %t/pun.c | FileCheck %s --check-prefix=PUN
// RUN: emitrust-import-c %t/untagged.c | FileCheck %s --check-prefix=UNTAGGED

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
// ALIAS: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"[[U]]">>
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
// UNTAGGED: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"[[U:[A-Za-z_][A-Za-z0-9_]*]]">>
// UNTAGGED: emitrust.member %{{.*}}["a"] : (!emitrust.lvalue<!emitrust.struct<"[[U]]">>) -> !emitrust.lvalue<i32>
// UNTAGGED-NOT: ["b"]

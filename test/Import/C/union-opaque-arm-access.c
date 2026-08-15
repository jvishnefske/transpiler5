// FR-83: an ARM ACCESS on an FR-78 opaque-union blob — u.<arm>.<field...>
// down to an INTEGER SCALAR leaf (including an array-ELEMENT leaf at a
// constant or runtime index), dot or arrow — lowers to a byte view of the
// blob at the leaf's clang-computed (ASTRecordLayout) offset: multi-byte
// leaves read via `T::from_ne_bytes` over the window's bytes and write via
// `T::to_ne_bytes` (the established CTS-P11 wide-byte image — per-byte
// gather into a fixed [u8; K], NO try_into/unwrap panic path), one-byte
// leaves are a single blob subscript. C's punning-through-arms falls out
// because every arm reads the same bytes: the blob IS the union's C
// memory. The IR op granularity for the byte view is GREEN's choice —
// these pins hold the blob projection (member["u"] -> member["opaque"],
// never an arm name), the ne_bytes spellings, the runtime-cursor scaling,
// and the staged-global traffic; the pun VALUES are pinned by byte-diff in
// test/EndToEnd/union-opaque-arm-pun.c. Everything NOT an integer-scalar
// leaf (whole-arm copies, array decay, float leaves, address-of an arm,
// arm initializers) stays a located rejection —
// union-opaque-aggregate-invalid.c pins that frontier, and the emitter
// backstop still refuses any unenumerated leak (errors.mlir).
// RUN: emitrust-import-c %s | FileCheck %s

union U {
  struct { unsigned a[4]; unsigned char z; } ip6;
  struct { unsigned a; } ip4;
}; /* dual-stack shape: 20-byte blob; ip6.a[k] at 4k, ip6.z at 16, ip4.a at 0 */

struct Rec {
  int tag;
  union U u;
};

struct Rec g;

// CHECK-DAG: emitrust.struct_def @[[U:([A-Za-z0-9_]+_)?U]] ["opaque"] [!emitrust.array<20xui8>] {emitrust.opaque_union}

// Arrow read, const offset 0, 4-byte leaf: blob projection then a
// from_ne_bytes combine; no arm name reaches the IR.
// CHECK-LABEL: func.func @read4
// CHECK: emitrust.member %{{.*}}["u"] : (!emitrust.lvalue<!emitrust.struct<"{{([A-Za-z0-9_]+_)?}}Rec">>) -> !emitrust.lvalue<!emitrust.struct<"[[U]]">>
// CHECK: emitrust.member %{{.*}}["opaque"] : (!emitrust.lvalue<!emitrust.struct<"[[U]]">>) -> !emitrust.lvalue<!emitrust.array<20xui8>>
// CHECK: emitrust.call_opaque "u32::from_ne_bytes"(%{{.*}}) : (!emitrust.array<4xui8>) -> ui32
unsigned read4(struct Rec *p) { return p->u.ip4.a; }

// Arrow write, const offset 0: the value splits with to_ne_bytes and the
// bytes scatter over the blob window.
// CHECK-LABEL: func.func @write4
// CHECK: emitrust.member %{{.*}}["opaque"]
// CHECK: emitrust.call_opaque "u32::to_ne_bytes"(%{{.*}}) : (ui32) -> !emitrust.array<4xui8>
void write4(struct Rec *p, unsigned v) { p->u.ip4.a = v; }

// Runtime array-element leaf (the dominant lwIP shape, ip6.addr[k]): the
// index converts to an i64 cursor scaled by the 4-byte element size.
// CHECK-LABEL: func.func @read6
// CHECK: emitrust.member %{{.*}}["opaque"]
// CHECK: arith.muli
// CHECK: emitrust.call_opaque "u32::from_ne_bytes"
unsigned read6(struct Rec *p, unsigned k) { return p->u.ip6.a[k]; }

// CHECK-LABEL: func.func @write6
// CHECK: arith.muli
// CHECK: emitrust.call_opaque "u32::to_ne_bytes"
void write6(struct Rec *p, unsigned k, unsigned v) { p->u.ip6.a[k] = v; }

// Constant element index folds into a compile-time cursor (a[3] = byte 12).
// CHECK-LABEL: func.func @read6c
// CHECK: arith.constant 12 : i64
// CHECK: emitrust.call_opaque "u32::from_ne_bytes"
unsigned read6c(struct Rec *p) { return p->u.ip6.a[3]; }

// One-byte leaf at const offset 16: a single blob subscript, no ne_bytes
// staging in either direction.
// CHECK-LABEL: func.func @readz
// CHECK: arith.constant 16 : i64
// CHECK: emitrust.subscript %{{.*}} : (!emitrust.lvalue<!emitrust.array<20xui8>>, i64) -> !emitrust.lvalue<ui8>
// CHECK-NOT: ne_bytes
// CHECK-LABEL: func.func @writez
// CHECK: arith.constant 16 : i64
// CHECK: emitrust.subscript %{{.*}} : (!emitrust.lvalue<!emitrust.array<20xui8>>, i64) -> !emitrust.lvalue<ui8>
// CHECK-NOT: ne_bytes
unsigned readz(struct Rec *p) { return p->u.ip6.z; }
void writez(struct Rec *p, unsigned char b) { p->u.ip6.z = b; }

// DOT access on a local, and the pun itself: write through the ip4 arm,
// read the same bytes back through the ip6 arm's element 0.
// CHECK-LABEL: func.func @local_pun
// CHECK: emitrust.variable named "r" : !emitrust.lvalue<!emitrust.struct<"{{([A-Za-z0-9_]+_)?}}Rec">>
// CHECK: emitrust.call_opaque "u32::to_ne_bytes"
// CHECK: emitrust.call_opaque "u32::from_ne_bytes"
unsigned local_pun(unsigned v) {
  struct Rec r = {0};
  r.u.ip4.a = v;
  return r.u.ip6.a[0];
}

// Compound assignment through an arm leaf is a read-modify-write over the
// same window: one from_ne_bytes load, the computation, one to_ne_bytes
// store.
// CHECK-LABEL: func.func @comp
// CHECK: emitrust.call_opaque "u32::from_ne_bytes"
// CHECK: emitrust.call_opaque "u32::to_ne_bytes"
void comp(struct Rec *p, unsigned k) { p->u.ip4.a += k; }

// ++ on a one-byte leaf: subscript load, add, subscript store — still no
// ne_bytes staging.
// CHECK-LABEL: func.func @bump
// CHECK: emitrust.subscript
// CHECK: emitrust.assign
// CHECK-NOT: ne_bytes
void bump(struct Rec *p) { p->u.ip6.z++; }

// An arm access on a GLOBAL record rides the ordinary staged-copy
// machinery: whole-value load, byte view on the staged copy, whole-value
// store-back on the write side.
// CHECK-LABEL: func.func @gw
// CHECK: emitrust.global_load @g
// CHECK: emitrust.call_opaque "u32::to_ne_bytes"
// CHECK: emitrust.global_store %{{.*}}, @g
void gw(unsigned v) { g.u.ip4.a = v; }

// CHECK-LABEL: func.func @gr
// CHECK: emitrust.global_load @g
// CHECK: emitrust.call_opaque "u32::from_ne_bytes"
// CHECK-NOT: emitrust.global_store
unsigned gr(void) { return g.u.ip4.a; }

// No arm spelling anywhere after the last body either.
// CHECK-NOT: ["ip4"]
// CHECK-NOT: ["ip6"]

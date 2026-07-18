// RUN: emitrust-import-c %s | FileCheck %s

// CTS-P9: `&struct.member` as a pointer-region base. The pointer-base
// binding carries an optional member path: taking the address of a
// scalar member of a local or global struct roots the region at that
// member. A local-struct member base resolves every access to the
// member's own place (`emitrust.member` on the struct variable, no
// runtime pointer state); a global-struct member base reuses the
// staged-copy + writeback machinery with a member projection (stage the
// whole global, project the member, store the whole value back). A
// region mixing a scalar base and a global-member base (the 00163 shape)
// extends the ordinary discriminant dispatch. Shapes outside the model
// stay rejected (see pointers-member-base-invalid.c).

// A member of a local struct: `p = &s.b` roots p at s.b, and every deref
// is s.b's own member place — no cell, no borrow, no address value.
struct P { int a; int b; };
int local_member(void) {
  struct P s;
  int *p;
  s.a = 1;
  p = &s.b;
  *p = 41;
  *p = *p + s.a;
  return s.b;
}
// CHECK-LABEL: func.func @local_member
// CHECK-NOT: memref.alloca
// CHECK: %[[S:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"P">>
// CHECK: %[[MA:.*]] = emitrust.member %[[S]]["a"]
// CHECK: emitrust.assign %[[MA]]
//   *p = 41 writes s.b's place directly.
// CHECK: %[[MB1:.*]] = emitrust.member %[[S]]["b"]
// CHECK: emitrust.assign %[[MB1]]
//   the compound *p = *p + s.a reads and writes the same member place.
// CHECK: arith.addi
// CHECK: emitrust.assign
//   return s.b observes the writes through the struct itself.
// CHECK: %[[MB2:.*]] = emitrust.member %[[S]]["b"]
// CHECK: %[[RET:.*]] = emitrust.load %[[MB2]]
// CHECK-NOT: emitrust.addr_of

// A member of a global struct: a write through `p = &gm.b` stages the
// whole global, assigns the projected member, and stores the whole value
// back; the following direct read stages afresh and observes it.
struct Z { int a; int b; int c; };
struct Z gm;
int global_member(int v) {
  int *p = &gm.b;
  *p = v;
  return gm.b;
}
// CHECK-LABEL: func.func @global_member
// CHECK: emitrust.global_load @gm : !emitrust.struct<"Z">
// CHECK: emitrust.member %{{.*}}["b"]
// CHECK: emitrust.assign
// CHECK: emitrust.global_store %{{.*}}, @gm : !emitrust.struct<"Z">
// CHECK: emitrust.global_load @gm : !emitrust.struct<"Z">
// CHECK: emitrust.member %{{.*}}["b"]
// CHECK: emitrust.load
// CHECK-NOT: emitrust.addr_of

// The 00163 shape: one pointer rebinding between a local scalar and a
// global struct's member. Both bases are degenerate scalars, so the
// pointer carries a discriminant cell only, and every deref dispatches:
// the scalar arm touches the local's place, the global-member arm goes
// through the staged copy with the member projection.
struct Z gmix;
int mixed_base(void) {
  int a = 42;
  int *b = &a;
  int r = *b;
  b = &gmix.b;
  *b = 34;
  return r + gmix.b;
}
// CHECK-LABEL: func.func @mixed_base
// CHECK: %[[A:.*]] = emitrust.variable : !emitrust.lvalue<i32>
//   b = &a stores discriminant 0.
// CHECK: memref.store %c0_i32{{[_0-9]*}}, %[[D:alloca[_0-9]*]][] : memref<i32>
//   r = *b dispatches on the discriminant.
// CHECK: %[[DV:.*]] = memref.load %[[D]][] : memref<i32>
// CHECK: arith.cmpi eq, %[[DV]], %{{.*}} : i32
// CHECK: cf.cond_br
//   the scalar arm reads a's own place.
// CHECK: emitrust.load %[[A]] : (!emitrust.lvalue<i32>) -> i32
//   the global-member arm stages the global and projects the member.
// CHECK: emitrust.global_load @gmix : !emitrust.struct<"Z">
// CHECK: emitrust.member %{{.*}}["b"]
//   b = &gmix.b rebinds: discriminant 1.
// CHECK: memref.store %c1_i32{{[_0-9]*}}, %[[D]][] : memref<i32>
//   the write flush's global-member arm stores the staged copy back.
// CHECK: emitrust.global_load @gmix : !emitrust.struct<"Z">
// CHECK: emitrust.member %{{.*}}["b"]
// CHECK: emitrust.assign
// CHECK: emitrust.global_store %{{.*}}, @gmix : !emitrust.struct<"Z">

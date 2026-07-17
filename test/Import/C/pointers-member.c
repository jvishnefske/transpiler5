// RUN: emitrust-import-c %s | FileCheck %s

// CTS-P2 (C99-43): a data-pointer struct member is stored as a plain i64
// cursor field — a cursor is a borrow-free Copy integer, so a struct can
// hold one — and the analysis resolves the member to one statically known
// target object (or string literal) per struct instance. Every supported
// binding is degenerate (the member designates one whole object), so the
// stored i64 carries no runtime information: member writes emit nothing,
// and member reads resolve to the bound object's place with no runtime
// state at all. Unresolvable shapes are located rejections (see
// pointers-member-invalid.c).

// A self-referential member (the 00019 shape): `s.p = &s` binds the
// member to the instance itself, and the `s.p->p->x` chain folds hop by
// hop back to `s.x` — no pointer value is ever materialized.
struct S { struct S *p; int x; };
int chain(void) {
  struct S s;
  s.x = 7;
  s.p = &s;
  return s.p->p->x;
}
// CHECK: emitrust.struct_def @S ["p", "x"] [i64, i32]
// CHECK-LABEL: func.func @chain
// CHECK: %[[s:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"S">>
// CHECK: %[[X1:.*]] = emitrust.member %[[s]]["x"]
// CHECK: emitrust.assign %[[X1]]
//   s.p = &s emits nothing; the chained read is a member of s directly.
// CHECK-NOT: emitrust.member %[[s]]["p"]
// CHECK: %[[X2:.*]] = emitrust.member %[[s]]["x"]
// CHECK: emitrust.load %[[X2]]
// CHECK: return

// A member bound to a sibling local: reads and writes through the member
// resolve to the target's own place.
int deref_local(void) {
  int t = 41;
  struct Q { int *ip; int pad; } q;
  q.ip = &t;
  *q.ip = *q.ip + 1;
  return t;
}
// CHECK-LABEL: func.func @deref_local
// CHECK: %[[T:.*]] = emitrust.variable : !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[T]]
//   *q.ip reads and writes t's place directly.
// CHECK: %[[V:.*]] = emitrust.load %[[T]]
// CHECK: %[[SUM:.*]] = arith.addi %[[V]]
// CHECK: emitrust.assign %[[T]] = %[[SUM]]
// CHECK: emitrust.struct_def @deref_local_Q ["ip", "pad"] [i64, i32]

// A pointer member of a global struct bound by its constant initializer
// (the 00049 shape): the stored i64 member initializes to 0 (the binding
// carries no runtime information), and `*g.gp` resolves to the bound
// global through the ordinary staged-copy access.
int gx = 10;
struct G { int a; int *gp; };
struct G g = { .gp = &gx, .a = 1 };
int global_member(void) { return *g.gp; }
// CHECK: emitrust.global @gx <10 : i32> : i32
// CHECK: emitrust.struct_def @G ["a", "gp"] [i32, i64]
// CHECK: emitrust.global @g <[1 : i32, 0]> : !emitrust.struct<"G">
// CHECK-LABEL: func.func @global_member
// CHECK: %[[STAGE:.*]] = emitrust.variable : !emitrust.lvalue<i32>
// CHECK: %[[CUR:.*]] = emitrust.global_load @gx : i32
// CHECK: emitrust.assign %[[STAGE]] = %[[CUR]]
// CHECK: emitrust.load %[[STAGE]]

// A write through the member of a global instance stores the staged copy
// back (the ordinary global write-back model).
void global_member_write(int v) { *g.gp = v; }
// CHECK-LABEL: func.func @global_member_write
// CHECK: emitrust.global_load @gx : i32
// CHECK: emitrust.global_store {{.*}}, @gx : i32

// A member bound to a string literal is write-only (nothing in the
// supported subset can observe it, the 00208 shape); the initializer
// stores the default 0 and emits no literal backing.
struct W { char *data; int n; };
int literal_member(void) {
  struct W w = { "bugs", 3 };
  return w.n;
}
// CHECK: emitrust.struct_def @W ["data", "n"] [i64, i32]
// CHECK-LABEL: func.func @literal_member
// CHECK-NOT: emitrust.variable const
// CHECK: %[[N:.*]] = emitrust.member {{.*}}["n"]
// CHECK: emitrust.assign %[[N]]

// A pointer member inside a pointer global's compound-literal backing
// (the 00150 shape): `go->pin` resolves through go's degenerate backing
// to the member's own bound global.
struct In { int a; int b; };
struct Out { struct In inner; struct In *pin; };
struct In gi = { 1, 2 };
struct Out *go = &(struct Out){ {3, 4}, &gi };
int compound_member(void) { return go->pin->b; }
// CHECK: emitrust.global @gi <[1 : i32, 2 : i32]> : !emitrust.struct<"In">
// CHECK: emitrust.global @go_backing <{{\[\[}}3 : i32, 4 : i32], 0]> : !emitrust.struct<"Out">
// CHECK-LABEL: func.func @compound_member
// CHECK: %[[GISTAGE:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"In">>
// CHECK: %[[GI:.*]] = emitrust.global_load @gi : !emitrust.struct<"In">
// CHECK: emitrust.assign %[[GISTAGE]] = %[[GI]]
// CHECK: emitrust.member %[[GISTAGE]]["b"]

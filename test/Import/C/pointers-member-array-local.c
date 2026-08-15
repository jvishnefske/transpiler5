// RUN: emitrust-import-c %s | FileCheck %s

// FR-93: pointer LOCALS bound to member arrays — the non-argument
// member-decay form every wave since FR-74 deliberately kept rejected
// ("pointer assigned a non-address value"). A local initialized from
// (or assigned) a TYPED member-array decay extends the (backing,
// cursor) local convention with a MEMBER-place backing: the base is the
// member's own place (projected fresh at every use, so no borrow is
// ever held across statements) and the i64 cursor cell counts elements
// from the member's start. Every existing local-pointer operation —
// deref, subscript, arithmetic, argument passing — composes over that
// backing exactly as over a top-level array; slice arguments borrow
// only the FIELD, keyed (root, field) in the call aliasing guard.
// This pins the init form, the assign (rebind-to-same-member) form,
// arrow roots (struct-pointer parameter) and dot roots (local struct),
// so a regression cannot silently fall back to the historical
// rejection or, worse, bind the cursor to the wrong backing.

struct S { int tag; int arr[4]; };

static void bump(int *xs, int n) {
  int i;
  for (i = 0; i < n; i++)
    xs[i] = xs[i] + 1;
}

// Arrow root: `p = s->arr` roots the region at member place s.arr with
// a member-relative cursor. Deref and subscript project the member
// fresh (deref the parameter reference, project "arr", subscript at
// the cursor); the assign form rebinds to the SAME (root, member) and
// only resets the cursor.
static int walk(struct S *s, int k) {
  int *p = s->arr;
  int a;
  p += k;
  a = *p + p[1];
  *p = 7;
  p = s->arr;
  return a + *p;
}
// CHECK-LABEL: func.func @walk
//   the cursor cell, initialized to 0 by the init binding.
// CHECK: %[[CELL:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[ZERO:.*]] = arith.constant 0 : i64
// CHECK: memref.store %[[ZERO]], %[[CELL]][]
//   p += k walks the cursor, no place is touched.
// CHECK: arith.addi
//   *p derefs the parameter, projects the member, subscripts at cursor.
// CHECK: %[[D1:.*]] = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.struct<"S">>
// CHECK: %[[M1:.*]] = emitrust.member %[[D1]]["arr"]
// CHECK: emitrust.subscript %[[M1]]
//   *p = 7 writes through the same member-place backing.
// CHECK: emitrust.member %{{.*}}["arr"]
// CHECK: emitrust.assign
//   the rebind `p = s->arr` stores cursor 0 again.
// CHECK: memref.store

// Slice-argument use: `bump(p, 2)` borrows ONLY the field — slice_of
// over the member place at the CURRENT cursor, the FR-74 (root, field)
// aliasing key.
static int arg_use(struct S *s) {
  int *p = s->arr;
  p += 1;
  bump(p, 2);
  return s->arr[1];
}
// CHECK-LABEL: func.func @arg_use
// CHECK: %[[D2:.*]] = emitrust.deref %arg0
// CHECK: %[[M2:.*]] = emitrust.member %[[D2]]["arr"]
// CHECK: emitrust.slice_of mut %[[M2]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
// CHECK: call @bump

// Dot root: a local struct's member array binds the same way; the
// member place projects on the struct variable directly.
static int dot_root(int k) {
  struct S s;
  int *q = s.arr;
  s.arr[0] = k;
  s.arr[1] = k + 1;
  q = q + 1;
  return *q;
}
// CHECK-LABEL: func.func @dot_root
// CHECK: %[[SV:.*]] = emitrust.variable named "s" : !emitrust.lvalue<!emitrust.struct<"S">>
// CHECK: %[[M3:.*]] = emitrust.member %[[SV]]["arr"]
// CHECK: emitrust.subscript %[[M3]]

int main(void) {
  struct S s;
  s.tag = 0;
  s.arr[0] = 1;
  s.arr[1] = 2;
  s.arr[2] = 3;
  s.arr[3] = 4;
  return walk(&s, 1) + arg_use(&s) + dot_root(2);
}

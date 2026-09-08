// RUN: emitrust-import-c %s | FileCheck %s

// Intent: pin the ADMITTED half of the negative-pointer-displacement
// invariant in the fast tier -- the owner (single-storage-base) lowering
// must keep carrying the displacement in the i64 cursor, and the fence in
// `refineElementPlace` must not reach it.
//
// The two lowerings differ in WHERE the pointer's position lives. Here the
// class unifies to exactly one local storage base (`a`), so `planOwners`
// promotes it: the parameter is an i64 element cursor into the receiver's
// WHOLE data array and `p[-1]` is `arith.addi` of the cursor and -1 before
// the subscript, which reaches a real element. The multi-object Phase-1b
// lowering instead re-bases a `&[T]` slice at the pointee, cannot address
// anything before it, and is refused -- see
// test/Import/C/pointer-param-negative-index-invalid.c. The runtime
// behavior of this file's shape is byte-diffed in
// test/EndToEnd/pointer-param-negative-index-owner.c; this test exists so a
// regression that quietly demotes the owner path (which would turn every
// program below into a refusal) fails without cargo.

int printf(const char *, ...);

static int prev(const int *p) { return p[-1]; }
// The subscript index is the parameter cursor PLUS a negative constant,
// computed in i64 before the subscript -- never a bare negative index.
// CHECK-LABEL: func.func @prev
// CHECK: %[[BASE:.*]] = emitrust.member %{{.*}}["data"] : (!emitrust.lvalue<!emitrust.struct<"Owner_main_a">>) -> !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK: %[[CUR:.*]] = memref.load
// CHECK: %[[NEG:.*]] = arith.subi
// CHECK: %[[EXT:.*]] = arith.extsi %[[NEG]]
// CHECK: %[[IDX:.*]] = arith.addi %[[CUR]], %[[EXT]] : i64
// CHECK: emitrust.subscript %[[BASE]][%[[IDX]]] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>

static int prev_deref(const int *p) { return *(p - 1); }
// The pointer-arithmetic spelling folds into the same cursor arithmetic.
// CHECK-LABEL: func.func @prev_deref
// CHECK: %[[BASE2:.*]] = emitrust.member %{{.*}}["data"]
// CHECK: %[[IDX2:.*]] = arith.subi %{{.*}}, %{{.*}} : i64
// CHECK: emitrust.subscript %[[BASE2]][%[[IDX2]]]

int main(void) {
  int a[4];
  int i;
  for (i = 0; i < 4; i++)
    a[i] = i;
  printf("%d %d\n", prev(&a[1]), prev_deref(&a[2]));
  return 0;
}

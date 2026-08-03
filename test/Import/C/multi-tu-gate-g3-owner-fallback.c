// W3.3 multi-TU gate oracle (G3 — FLIPPED to promotion): planOwners'
// owner-struct promotion (FR-30) used to require every unified function
// to be non-externally-visible unless this TU was the whole program
// (ImportCPlanning.cpp), because another TU could call an externally
// visible function with an argument this TU never sees — breaking the
// all-or-nothing per-function promotion rule. W3.3 G3 relaxes that gate
// with the W3.2 WholeProgramInfo facts: an externally visible function is
// promoted in a multi-TU project when the whole-program call/address-taken
// enumeration proves NO other TU references it, so this TU has the same
// sole-TU-equivalent visibility the rule needs. This is a suboptimality
// gate — the fallback was always correct — so the promotion is a pure
// optimization; the multi-file program still behaves identically (its
// EndToEnd differential is multi-tu-gate-g3-owner-fallback in
// test/EndToEnd).
//
// This file is exactly test/Import/C/owners.c's content (the FR-30
// single-TU positive) plus a trivial companion TU (`unrelated_g3`, which
// never calls fill/sum/total) that only forces the >=2-TU project import
// path. With W3.3 G3, fill/sum/total — externally visible, referenced
// only by this TU — now promote to the SAME Owner_main_arr struct/method
// shape owners.c asserts for the sole-TU case. The multi-base
// counterexample stays on the slice fallback: see
// multi-tu-gate-g3-owner-multibase.c.
//
// RUN: emitrust-import-c %s %S/Inputs/multi-tu-gate-g3-owner-fallback-other.c | FileCheck %s

// A subscripting callee becomes a method: the pointer parameter is an i64
// index whose cursor decomposes against the receiver's data member.
void fill(int *p, int n) {
  for (int i = 0; i < n; i++) {
    p[i] = i;
  }
}
// CHECK-LABEL: func.func @fill
// CHECK-SAME: (%[[SELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, %[[IDX:.*]]: i64, %{{.*}}: i32)
// CHECK-SAME: attributes {emitrust.method_of = "Owner_main_arr"}
// CHECK: %[[RCV:.*]] = emitrust.deref %[[SELF]] : (!emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>
// CHECK: %[[DATA:.*]] = emitrust.member %[[RCV]]["data"] : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.array<8xi32>>
// CHECK: emitrust.subscript %[[DATA]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<8xi32>>, i64) -> !emitrust.lvalue<i32>

// A walking callee: the cursor cell updates exactly like a pointer local.
int sum(int *p, int n) {
  int s = 0;
  while (n > 0) {
    s = s + *p;
    p++;
    n--;
  }
  return s;
}
// CHECK-LABEL: func.func @sum
// CHECK-SAME: (%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, %{{.*}}: i64, %{{.*}}: i32) -> i32
// CHECK-SAME: attributes {emitrust.method_of = "Owner_main_arr"}
// CHECK: emitrust.member %{{.*}}["data"]

// A sibling method call: the receiver is the method's own dereferenced
// receiver, and the pointer argument lowers to its loaded cursor.
int total(int *p, int n) {
  return sum(p, n);
}
// CHECK-LABEL: func.func @total
// CHECK-SAME: attributes {emitrust.method_of = "Owner_main_arr"}
// CHECK: %[[TREF:.*]] = emitrust.addr_of mut %{{.*}} : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>
// CHECK: call @sum(%[[TREF]], %{{.*}}, %{{.*}}) {emitrust.method_call}

// CHECK-LABEL: func.func @c_main
int main(void) {
  // The owner base declares the struct variable; direct accesses rewrite
  // to member("data") + subscript, and no whole-struct load ever appears.
  // CHECK: %[[OWNER:.*]] = emitrust.variable named "arr" : !emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>
  // CHECK: %[[MDATA:.*]] = emitrust.member %[[OWNER]]["data"] : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.array<8xi32>>
  int arr[8];
  // A decayed-array argument lowers to the constant cursor 0.
  // CHECK: %[[REF0:.*]] = emitrust.addr_of mut %[[OWNER]] : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>
  // CHECK: call @fill(%[[REF0]], %{{.*}}, %{{.*}}) {emitrust.method_call} : (!emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, i64, i32) -> ()
  fill(arr, 8);
  // An interior pointer &arr[2] lowers to cursor 2.
  // CHECK: %[[REF1:.*]] = emitrust.addr_of mut %[[OWNER]]
  // CHECK: call @sum(%[[REF1]], %{{.*}}, %{{.*}}) {emitrust.method_call}
  int t = sum(&arr[2], 4);
  // A direct element read of the owner routes through the data member.
  // CHECK: emitrust.subscript %[[MDATA]][%{{.*}}]
  // CHECK: call @total({{.*}}) {emitrust.method_call}
  t = t + total(arr, arr[0]);
  return t;
}
// CHECK-NOT: emitrust.load %[[OWNER]]

// The owner struct is synthesized at module level.
// CHECK: emitrust.struct_def @Owner_main_arr ["data"] [!emitrust.array<8xi32>]

// The unrelated companion TU function is imported normally (it forces the
// multi-file path but references nothing the promotion depends on).
// CHECK: func.func @unrelated_g3

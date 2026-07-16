// RUN: emitrust-import-c %s | FileCheck %s

// FR-30: Phase-4 owner structs / active objects. An interprocedural
// pointer-region analysis (pure AST, run before any IR is built) unifies
// each pointer call argument's root object with the callee's parameter; a
// region with a single non-escaping local array base of at most 32
// elements that crosses a function boundary — with every unified function
// defined, and every call site visible, in this translation unit — is
// promoted: the base becomes a module-level `Owner_<fn>_<base>` struct
// holding the array in its "data" field, every unified function becomes a
// method (`emitrust.method_of` attribute; the receiver leads the signature
// and pointer parameters lower to i64 element indices decomposed against
// `deref(arg0) -> member("data")`), and call sites borrow the owner for
// exactly one `emitrust.method_call`-tagged `func.call`.

// A subscripting callee: the pointer parameter becomes an i64 index whose
// cursor decomposes against the receiver's data member.
void fill(int *p, int n) {
  for (int i = 0; i < n; i++) {
    p[i] = i;
  }
}
// CHECK-LABEL: func.func @fill
// CHECK-SAME: (%[[SELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, %[[IDX:.*]]: i64, %{{.*}}: i32)
// CHECK-SAME: attributes {emitrust.method_of = "Owner_main_arr"}
// CHECK: %[[CUR:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[RCV:.*]] = emitrust.deref %[[SELF]] : (!emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>
// CHECK: %[[DATA:.*]] = emitrust.member %[[RCV]]["data"] : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.array<8xi32>>
// CHECK: memref.store %[[IDX]], %[[CUR]][] : memref<i64>
//   p[i] subscripts the data member at cursor + i.
// CHECK: %[[SUM:.*]] = arith.addi
// CHECK: emitrust.subscript %[[DATA]][%[[SUM]]] : (!emitrust.lvalue<!emitrust.array<8xi32>>, i64) -> !emitrust.lvalue<i32>

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
// CHECK: %[[TRCV:.*]] = emitrust.deref %{{.*}} : (!emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>
// CHECK: %[[TCUR:.*]] = memref.load %{{.*}}[] : memref<i64>
// CHECK: %[[TREF:.*]] = emitrust.addr_of mut %[[TRCV]] : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>
// CHECK: call @sum(%[[TREF]], %[[TCUR]], %{{.*}}) {emitrust.method_call}

// CHECK-LABEL: func.func @c_main
int main(void) {
  // The owner base declares the struct variable; direct accesses rewrite
  // to member("data") + subscript, and no whole-struct load ever appears.
  // CHECK: %[[OWNER:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>
  // CHECK: %[[MDATA:.*]] = emitrust.member %[[OWNER]]["data"] : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.array<8xi32>>
  int arr[8];
  // A decayed-array argument lowers to the constant cursor 0; the value
  // argument is materialized before the single receiver borrow.
  // CHECK: %[[C0:.*]] = arith.constant 0 : i64
  // CHECK: %[[N8:.*]] = arith.constant 8 : i32
  // CHECK: %[[REF0:.*]] = emitrust.addr_of mut %[[OWNER]] : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>
  // CHECK: call @fill(%[[REF0]], %[[C0]], %[[N8]]) {emitrust.method_call} : (!emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, i64, i32) -> ()
  fill(arr, 8);
  // An interior pointer &arr[2] lowers to cursor 2.
  // CHECK: arith.addi
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

// The owner struct is synthesized at module level (after the imported
// functions), named after the C spellings of the owning function and the
// base variable.
// CHECK: emitrust.struct_def @Owner_main_arr ["data"] [!emitrust.array<8xi32>]

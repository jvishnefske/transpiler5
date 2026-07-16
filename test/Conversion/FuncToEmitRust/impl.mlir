// FR-30: convert-func-to-emitrust materializes the owner-struct surface:
// a func.func tagged `emitrust.method_of` lands inside a lazily created
// emitrust.impl for the named struct (one impl per owner, tag stripped),
// and a func.call tagged `emitrust.method_call` — whose first operand is
// the `emitrust.addr_of mut` borrow of the owner place — becomes an
// emitrust.method_call on that place, with the dead borrow erased.
// RUN: emitrust-opt --convert-func-to-emitrust %s | FileCheck %s

// CHECK-LABEL: emitrust.func @c_main
// CHECK:         %[[O:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>
// CHECK-NOT:     emitrust.addr_of
// CHECK:         emitrust.method_call %[[O]]["fill"] (%{{.*}}) : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>, i64) -> ()
// CHECK:         %[[R:.*]] = emitrust.method_call %[[O]]["sum"] (%{{.*}}) : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>, i64) -> i32
// CHECK:         emitrust.return %[[R]] : i32
func.func @c_main() -> i32 {
  %o = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>
  %i = arith.constant 0 : i64
  %r0 = emitrust.addr_of mut %o : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>
  call @fill(%r0, %i) {emitrust.method_call} : (!emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, i64) -> ()
  %r1 = emitrust.addr_of mut %o : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>
  %s = call @sum(%r1, %i) {emitrust.method_call} : (!emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, i64) -> i32
  return %s : i32
}

// Both tagged functions land inside one impl, stripped of the tag; a
// sibling method call through the dereferenced receiver stays a
// method_call on the deref place.
// CHECK:       emitrust.impl "Owner_main_arr" {
// CHECK-NOT:   emitrust.method_of
// CHECK:         emitrust.func @fill(%[[SELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, %[[I:.*]]: i64)
// CHECK:           %[[RCV:.*]] = emitrust.deref %[[SELF]]
// CHECK-NOT:       emitrust.addr_of
// CHECK:           emitrust.method_call %[[RCV]]["sum"] (%[[I]]) : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>, i64) -> i32
// CHECK:         emitrust.func @sum(%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, %{{.*}}: i64) -> i32
// CHECK:       }
func.func @fill(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, %arg1: i64) attributes {emitrust.method_of = "Owner_main_arr"} {
  %s = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>
  %r = emitrust.addr_of mut %s : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>
  %v = call @sum(%r, %arg1) {emitrust.method_call} : (!emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, i64) -> i32
  return
}

func.func @sum(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, %arg1: i64) -> i32 attributes {emitrust.method_of = "Owner_main_arr"} {
  %c = arith.constant 0 : i32
  return %c : i32
}

// CHECK-NOT: func.func
// CHECK-NOT: func.call

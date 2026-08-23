// FR-121 (emitter half): a NON-const array variable whose element is
// mutably borrowed through a subscript projection must render `let mut`.
// `emitVariable`'s decision is `!isConst && lvalueIsMutated(..)`, and
// `lvalueIsMutated` must see the `addr_of mut` THROUGH the `subscript`
// (the refinesBase projection walk) -- if either half regresses, the
// rendered `let` loses its `mut` and the borrow is rustc E0596, an
// emitted crate that cannot compile. This is the borrow-position sibling
// of loop-deferred-mut.mlir's assignment-position pin. The const twin in
// the same function pins the other direction: a const backing that is
// only read stays a plain `let` (no silent mut-widening of const data).
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s
module {
  emitrust.func @take(%arg0: !emitrust.mut_ref<i8>) -> i32 attributes {emitrust.param_names = ["p"]} {
    %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<i8>) -> !emitrust.lvalue<i8>
    %1 = emitrust.load %0 : (!emitrust.lvalue<i8>) -> i8
    %2 = emitrust.cast %1 : i8 to i32
    emitrust.return %2 : i32
  }
  emitrust.func @c_main() -> i32 {
    %0 = emitrust.constant <0 : i64> : i64
    // CHECK: let v1: [i8; 3] = [65, 122, 0];
    %1 = emitrust.variable const <[65 : i8, 122 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<3xi8>>
    %2 = emitrust.subscript %1[%0] : (!emitrust.lvalue<!emitrust.array<3xi8>>, i64) -> !emitrust.lvalue<i8>
    %3 = emitrust.load %2 : (!emitrust.lvalue<i8>) -> i8
    // CHECK: let mut v3: [i8; 3] = [65, 122, 0];
    %4 = emitrust.variable <[65 : i8, 122 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<3xi8>>
    %5 = emitrust.subscript %4[%0] : (!emitrust.lvalue<!emitrust.array<3xi8>>, i64) -> !emitrust.lvalue<i8>
    // CHECK: = &mut v3[
    %6 = emitrust.addr_of mut %5 : (!emitrust.lvalue<i8>) -> !emitrust.mut_ref<i8>
    %7 = emitrust.call_opaque "take"(%6) : (!emitrust.mut_ref<i8>) -> i32
    %8 = emitrust.cast %3 : i8 to i32
    %9 = emitrust.add %7, %8 : i32
    emitrust.return %9 : i32
  }
}

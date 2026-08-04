// Global accessors inside an owner impl resolve module-level globals.
// This pins the fix for the pre-existing `'TOTAL' does not reference a
// valid emitrust.global` bug (recorded off-slice by FR-62 slice 5b): the
// FR-30 owner promotion moves a function into an `emitrust.impl` block,
// and `emitrust.impl` carries the SymbolTable trait, so a nearest-table
// lookup from a nested emitrust.global_load / global_store / global_cells
// resolved in the impl's (empty-of-globals) table and never saw the
// module-level `emitrust.global`. Like DataEnumDefOp::lookupFrom (FR-62
// slice 5a), the accessors must resolve in the ENCLOSING MODULE's symbol
// table. All three accessor ops are exercised inside a method; the file
// must verify and reprint byte-stably through a second emitrust-opt.
// RUN: emitrust-opt %s | emitrust-opt | FileCheck %s

// CHECK: emitrust.global @total <0 : i32> : i32
emitrust.global @total <0 : i32> : i32
// CHECK: emitrust.global @table : !emitrust.array<4xi32>
emitrust.global @table : !emitrust.array<4xi32>

emitrust.struct_def @Owner_main_values ["data"] [!emitrust.array<4xi32>]

// CHECK: emitrust.impl "Owner_main_values"
emitrust.impl "Owner_main_values" {
  // CHECK-LABEL: emitrust.func @add_from
  emitrust.func @add_from(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner_main_values">>, %arg1: i64) {
    %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Owner_main_values">>) -> !emitrust.lvalue<!emitrust.struct<"Owner_main_values">>
    %1 = emitrust.member %0["data"] : (!emitrust.lvalue<!emitrust.struct<"Owner_main_values">>) -> !emitrust.lvalue<!emitrust.array<4xi32>>
    %2 = emitrust.subscript %1[%arg1] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
    %3 = emitrust.load %2 : (!emitrust.lvalue<i32>) -> i32
    // A load inside the impl's nested symbol table sees the module global.
    // CHECK: emitrust.global_load @total : i32
    %4 = emitrust.global_load @total : i32
    %5 = emitrust.add %4, %3 : i32
    // CHECK: emitrust.global_store %{{.*}}, @total : i32
    emitrust.global_store %5, @total : i32
    // The cell-slice borrow of an array global resolves the same way.
    // CHECK: emitrust.global_cells @table
    emitrust.global_cells @table {
    ^bb0(%cells: !emitrust.ref<!emitrust.cell_slice<i32>>):
      // CHECK: emitrust.cell_get
      %v = emitrust.cell_get %cells[%arg1] : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64) -> i32
      // CHECK: emitrust.cell_set
      emitrust.cell_set %cells[%arg1], %v : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64, i32) -> ()
      emitrust.yield
    }
    emitrust.return
  }
}

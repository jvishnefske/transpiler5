// FR-83: the FR-78 opaque-union marker backstop gains its ONE enumerated
// allowance — an `emitrust.member` on a marked struct_def that selects the
// struct's single BLOB field (cross-checked by name against the
// struct_def, so the E0609 hazard the backstop exists for cannot recur)
// renders like any other member place: `.opaque[i]` byte views are how
// FR-83 lowers arm accesses. Every OTHER member name on a marked type
// still refuses at emission (errors.mlir pins the arm-name leak and the
// no-single-blob cross-check failure); this file pins the positive side.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

emitrust.struct_def @U ["opaque"] [!emitrust.array<8xui8>] {emitrust.opaque_union}
emitrust.struct_def @Rec ["u"] [!emitrust.struct<"U">]

// CHECK: fn peek(p: &mut Rec) -> u8 {
// CHECK: p.u.opaque[3i64 as usize]
emitrust.func @peek(%arg0: !emitrust.mut_ref<!emitrust.struct<"Rec">>) -> ui8 attributes {emitrust.param_names = ["p"]} {
  %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Rec">>) -> !emitrust.lvalue<!emitrust.struct<"Rec">>
  %1 = emitrust.member %0["u"] : (!emitrust.lvalue<!emitrust.struct<"Rec">>) -> !emitrust.lvalue<!emitrust.struct<"U">>
  %2 = emitrust.member %1["opaque"] : (!emitrust.lvalue<!emitrust.struct<"U">>) -> !emitrust.lvalue<!emitrust.array<8xui8>>
  %3 = emitrust.constant <3 : i64> : i64
  %4 = emitrust.subscript %2[%3] : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.lvalue<ui8>
  %5 = emitrust.load %4 : (!emitrust.lvalue<ui8>) -> ui8
  emitrust.return %5 : ui8
}

// CHECK: fn poke(_p: &mut Rec, b: u8) {
// CHECK: _p.u.opaque[0i64 as usize] = b;
emitrust.func @poke(%arg0: !emitrust.mut_ref<!emitrust.struct<"Rec">>, %arg1: ui8) attributes {emitrust.param_names = ["p", "b"]} {
  %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Rec">>) -> !emitrust.lvalue<!emitrust.struct<"Rec">>
  %1 = emitrust.member %0["u"] : (!emitrust.lvalue<!emitrust.struct<"Rec">>) -> !emitrust.lvalue<!emitrust.struct<"U">>
  %2 = emitrust.member %1["opaque"] : (!emitrust.lvalue<!emitrust.struct<"U">>) -> !emitrust.lvalue<!emitrust.array<8xui8>>
  %3 = emitrust.constant <0 : i64> : i64
  %4 = emitrust.subscript %2[%3] : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.lvalue<ui8>
  emitrust.assign %4 = %arg1 : !emitrust.lvalue<ui8>
  emitrust.return
}

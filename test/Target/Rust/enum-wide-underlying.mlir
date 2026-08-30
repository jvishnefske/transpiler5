// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// FR-166: the open enum's tuple-struct storage follows the `wide_underlying`
// marker, so the four combinations of {narrow, wide} x {signed, unsigned}
// spell i32/u32/i64/u64 -- and the SAME four spellings must appear on the
// ` as T` tail of the integer-to-enum constructor, because that tail feeds
// the tuple struct's one field. A mismatch between the two sites is a rustc
// type error rather than a miscompile, but the pin is byte-level on purpose:
// the narrow rows exist so that any future edit that shifts a byte of the
// PRE-FR-166 spelling fails here instead of silently rewriting every emitted
// crate in the suite.
//
// The identity-tail rule (FR-63, `rendersAsEnumUnderlying`) follows the
// width too: `i64 -> Jt` drops its ` as i64` because the operand already
// renders at the field's type, while `i64 -> Uw` keeps ` as u64` -- a real
// signedness conversion.

emitrust.enum_def @Jt ["A", "Big"] [0, 5000000000] {wide_underlying}
emitrust.enum_def @Uw ["A", "Big"] [0, 5000000000] {unsigned_underlying, wide_underlying}
emitrust.enum_def @Ni ["A", "B"] [0, 1]
emitrust.enum_def @Nu ["A", "B"] [0, 1] {unsigned_underlying}

// CHECK:      struct Jt(i64);
// CHECK:          const A: Jt = Jt(0);
// CHECK-NEXT:     const Big: Jt = Jt(5000000000);
// CHECK:      struct Uw(u64);
// CHECK:          const A: Uw = Uw(0);
// CHECK-NEXT:     const Big: Uw = Uw(5000000000);
// CHECK:      struct Ni(i32);
// CHECK:      struct Nu(u32);

// The constructor argument converts to the tuple struct's one field, so it
// carries the SAME four spellings; the ` as T` tail drops (FR-63) exactly
// when the operand already renders at that field's type.
// CHECK-LABEL: fn ctor(v0: i64, v1: i32) {
// CHECK-NEXT:    sink4(Jt(v0), Uw(v0 as u64), Ni(v1), Nu(v1 as u32));
// CHECK-NEXT:  }
emitrust.func @ctor(%arg0: i64, %arg1: i32) {
  %0 = emitrust.cast %arg0 : i64 to !emitrust.enum<"Jt">
  %1 = emitrust.cast %arg0 : i64 to !emitrust.enum<"Uw">
  %2 = emitrust.cast %arg1 : i32 to !emitrust.enum<"Ni">
  %3 = emitrust.cast %arg1 : i32 to !emitrust.enum<"Nu">
  emitrust.call_opaque "sink4"(%0, %1, %2, %3)
      : (!emitrust.enum<"Jt">, !emitrust.enum<"Uw">, !emitrust.enum<"Ni">,
         !emitrust.enum<"Nu">) -> ()
  emitrust.return
}

// Reading the raw field back: `Jt`'s field IS `i64`, so `as i64` drops;
// `Uw`'s is `u64`, so the same target keeps a real signedness cast; and a
// NARROWING read off a wide enum keeps its cast too -- that one is the
// truncation `castEnumToI32` must never emit implicitly.
// CHECK-LABEL: fn raw(v0: Jt, v1: Uw) {
// CHECK-NEXT:    sink3(v0.0, v1.0 as i64, v0.0 as i32);
// CHECK-NEXT:  }
emitrust.func @raw(%arg0: !emitrust.enum<"Jt">, %arg1: !emitrust.enum<"Uw">) {
  %0 = emitrust.cast %arg0 : !emitrust.enum<"Jt"> to i64
  %1 = emitrust.cast %arg1 : !emitrust.enum<"Uw"> to i64
  %2 = emitrust.cast %arg0 : !emitrust.enum<"Jt"> to i32
  emitrust.call_opaque "sink3"(%0, %1, %2) : (i64, i64, i32) -> ()
  emitrust.return
}

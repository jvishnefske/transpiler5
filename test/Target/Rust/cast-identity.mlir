// FR-63 (clippy::unnecessary_cast): a cast whose ` as T` tail converts the
// rendered source to the type it ALREADY has is the identity, so the
// emitter drops the tail and renders the source alone. This test pins the
// drop's exact scope and, critically, its classification: the rendered
// text is the source's, so the captured rank and trailing-cast flag must
// be INHERITED from the source -- a stale Cast-rank classification would
// drop parens a looser inherited rank still needs (`(a - b) as i32` under
// `*` must keep `(a - b) * c`, never `a - b * c`), and a stale
// trailing-cast flag in the other direction would lose the parens that
// guard rustc's generic-args misparse (`(x as u32) < y`). Rendered-type
// equality is the authority (signless and signed integers both spell
// `iN`); the two enum shapes fold against the open enum's raw field: the
// `.0` read drops its tail only when the target IS the field's u32/i32
// underlying, and the constructor argument drops its ` as u32/i32` only
// when the operand already has the underlying type. The drop is SPELLING
// only -- the computed value is bit-identical -- and deliberately narrow:
// any width or signedness change, a bool source, and the cross-signedness
// enum reads keep today's cast exactly.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

emitrust.enum_def @Sig ["Off", "On"] [0, 1]
emitrust.enum_def @Flag ["Off", "On"] [0, 1] {unsigned_underlying}

// The signed and unsigned integer identities drop; the source inlines into
// the consumer exactly as the bare name would.
// CHECK-LABEL: fn identity_int(v0: i32, v1: u32) {
// CHECK-NEXT:    sink2(v0, v1);
// CHECK-NEXT:  }
emitrust.func @identity_int(%arg0: i32, %arg1: ui32) {
  %a = emitrust.cast %arg0 : i32 to i32
  %b = emitrust.cast %arg1 : ui32 to ui32
  emitrust.call_opaque "sink2"(%a, %b) : (i32, ui32) -> ()
  emitrust.return
}

// Narrowness: a width change, a signedness change, and a bool source are
// REAL conversions and keep the cast exactly.
// CHECK-LABEL: fn real_casts(v0: i32, v1: u32) {
// CHECK-NEXT:    sink3(v0 as i64, v1 as i32, (v0 < 0i32) as i32);
// CHECK-NEXT:  }
emitrust.func @real_casts(%arg0: i32, %arg1: ui32) {
  %w = emitrust.cast %arg0 : i32 to i64
  %s = emitrust.cast %arg1 : ui32 to i32
  %z = emitrust.constant <0 : i32> : i32
  %c = emitrust.cmp lt, %arg0, %z : (i32, i32) -> i1
  %b = emitrust.cast %c : i1 to i32
  emitrust.call_opaque "sink3"(%w, %s, %b) : (i64, i32, i32) -> ()
  emitrust.return
}

// A chain drops ONLY the outer identity: the inner signedness conversion
// survives, and the resulting text still ends in that real cast.
// CHECK-LABEL: fn chain_outer_identity(v0: i32) {
// CHECK-NEXT:    sink(v0 as u32);
// CHECK-NEXT:  }
emitrust.func @chain_outer_identity(%arg0: i32) {
  %inner = emitrust.cast %arg0 : i32 to ui32
  %outer = emitrust.cast %inner : ui32 to ui32
  emitrust.call_opaque "sink"(%outer) : (ui32) -> ()
  emitrust.return
}

// Inherited trailing-cast flag, the misparse guard: the dropped identity
// over a chain ends in the surviving REAL cast, so directly left of `<`
// it must parenthesize (rustc parses `v0 as u32 < v1` as generic
// arguments on the type -- a hard error).
// CHECK-LABEL: fn ends_in_cast_lt(v0: i32, v1: u32) {
// CHECK-NEXT:    sink((v0 as u32) < v1);
// CHECK-NEXT:  }
emitrust.func @ends_in_cast_lt(%arg0: i32, %arg1: ui32) {
  %inner = emitrust.cast %arg0 : i32 to ui32
  %outer = emitrust.cast %inner : ui32 to ui32
  %r = emitrust.cmp lt, %outer, %arg1 : (ui32, ui32) -> i1
  emitrust.call_opaque "sink"(%r) : (i1) -> ()
  emitrust.return
}

// ... and the flag clears when the dropped identity leaves a bare atom:
// no parens under the same `<`.
// CHECK-LABEL: fn atom_lt(v0: i32, v1: i32) {
// CHECK-NEXT:    sink(v0 < v1);
// CHECK-NEXT:  }
emitrust.func @atom_lt(%arg0: i32, %arg1: i32) {
  %c = emitrust.cast %arg0 : i32 to i32
  %r = emitrust.cmp lt, %c, %arg1 : (i32, i32) -> i1
  emitrust.call_opaque "sink"(%r) : (i1) -> ()
  emitrust.return
}

// Inherited rank, the fold's risk center: a dropped `(a - b) as i32`
// carries AddSub rank, LOOSER than the consuming `*`, so the parens must
// survive. A stale Cast-rank classification would emit `v0 - v1 * v2` and
// regroup the expression -- a miscompile.
// CHECK-LABEL: fn prec_loose(v0: i32, v1: i32, v2: i32) {
// CHECK-NEXT:    sink((v0 - v1) * v2);
// CHECK-NEXT:  }
emitrust.func @prec_loose(%arg0: i32, %arg1: i32, %arg2: i32) {
  %d = emitrust.sub %arg0, %arg1 : i32
  %c = emitrust.cast %d : i32 to i32
  %m = emitrust.mul %c, %arg2 : i32
  emitrust.call_opaque "sink"(%m) : (i32) -> ()
  emitrust.return
}

// A MULTI-use dropped cast keeps its own binding; the right-hand side is
// the bare source and the annotation is the (identical) result type.
// CHECK-LABEL: fn multi_use(v0: i32) {
// CHECK-NEXT:    let v1: i32 = v0;
// CHECK-NEXT:    sink2(v1, v1);
// CHECK-NEXT:  }
emitrust.func @multi_use(%arg0: i32) {
  %c = emitrust.cast %arg0 : i32 to i32
  emitrust.call_opaque "sink2"(%c, %c) : (i32, i32) -> ()
  emitrust.return
}

// Enum raw reads: the `.0` field IS the underlying type, so the matching
// target drops its tail on both the signed and unsigned defs, and the
// bare postfix chain takes a method receiver with no parens.
// CHECK-LABEL: fn enum_raw_identity(v0: Sig, v1: Flag) {
// CHECK-NEXT:    sink3(v0.0, v1.0, v1.0.wrapping_add(1u32));
// CHECK-NEXT:  }
emitrust.func @enum_raw_identity(%arg0: !emitrust.enum<"Sig">, %arg1: !emitrust.enum<"Flag">) {
  %s = emitrust.cast %arg0 : !emitrust.enum<"Sig"> to i32
  %u = emitrust.cast %arg1 : !emitrust.enum<"Flag"> to ui32
  %u2 = emitrust.cast %arg1 : !emitrust.enum<"Flag"> to ui32
  %one = emitrust.constant <1 : ui32> : ui32
  %a = emitrust.add %u2, %one : ui32
  emitrust.call_opaque "sink3"(%s, %u, %a) : (i32, ui32, ui32) -> ()
  emitrust.return
}

// Cross-signedness and widening enum reads are REAL conversions on the
// raw field and keep the cast exactly (the C99-5 unsigned-underlying
// comparisons depend on `c.0 as i32` over a u32 field converting).
// CHECK-LABEL: fn enum_raw_real(v0: Sig, v1: Flag) {
// CHECK-NEXT:    sink3(v0.0 as u32, v1.0 as i32, v0.0 as i64);
// CHECK-NEXT:  }
emitrust.func @enum_raw_real(%arg0: !emitrust.enum<"Sig">, %arg1: !emitrust.enum<"Flag">) {
  %a = emitrust.cast %arg0 : !emitrust.enum<"Sig"> to ui32
  %b = emitrust.cast %arg1 : !emitrust.enum<"Flag"> to i32
  %c = emitrust.cast %arg0 : !emitrust.enum<"Sig"> to i64
  emitrust.call_opaque "sink3"(%a, %b, %c) : (ui32, i32, i64) -> ()
  emitrust.return
}

// Constructor argument: an operand already at the underlying type sheds
// the ` as i32` AND its cast-source parens (the ctor parens delimit); a
// signedness conversion keeps the cast.
// CHECK-LABEL: fn enum_ctor(v0: i32) {
// CHECK-NEXT:    sink2(Sig(v0 - 4i32), Flag(v0 as u32));
// CHECK-NEXT:  }
emitrust.func @enum_ctor(%arg0: i32) {
  %four = emitrust.constant <4 : i32> : i32
  %d = emitrust.sub %arg0, %four : i32
  %s = emitrust.cast %d : i32 to !emitrust.enum<"Sig">
  %f = emitrust.cast %arg0 : i32 to !emitrust.enum<"Flag">
  emitrust.call_opaque "sink2"(%s, %f) : (!emitrust.enum<"Sig">, !emitrust.enum<"Flag">) -> ()
  emitrust.return
}

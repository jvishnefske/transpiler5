// FR-173 D3: the GENERAL structural backstop behind FR-78's marker check.
//
// FR-78 refuses a leaked opaque-union arm access, but it only inspects
// `emitrust.member` ops whose BASE TYPE is the marked union struct_def. A
// selection made against the PARENT struct therefore sailed straight
// through: measured at HEAD, a module whose `struct S` has fields
// ["cmd", "u"] and whose member op selects "opaque" on it translated with
// rc=0 and rendered `v0.opaque[0usize]` -- rustc E0609, a whole-crate loss
// with no source location, which is exactly the failure mode FR-78 exists
// to stop. The marker check is one special case of a general rule, so the
// general rule is what this file pins: every `emitrust.member` whose base
// names a struct_def OF THIS MODULE must select a field that def actually
// has. Being general, it admits a whole CLASS of leaks rather than the one
// shape a marker happens to tag.
//
// The ORDER of the two checks is load-bearing and is pinned here too. An
// ARM selection on a marked union violates BOTH rules at once; FR-78's
// wording names the contract that was broken and the obligation the
// importer failed ("must reject this at the access site"), so it has to
// keep firing first. Placed ahead of it, this backstop shadows that
// message -- and test/Target/Rust/errors.mlir's pin of it. The
// `--implicit-check-not` on the first RUN line is what fails if the
// ordering ever inverts: no unit may ever report the STRUCTURAL wording
// against the marked union 'U'.
//
// A struct type this module carries NO def for is not this check's
// business at all: an extern/opaque record legitimately has no field list
// here, and refusing it would reject working modules. The first unit pins
// that it still translates, byte for byte.

// RUN: not emitrust-translate --mlir-to-rust --split-input-file %s 2>&1 \
// RUN:   | FileCheck %s --implicit-check-not="does not exist on struct 'U'"
// RUN: not emitrust-translate --mlir-to-rust --split-input-file %s 2>/dev/null \
// RUN:   | FileCheck %s --check-prefix=POS --strict-whitespace

// The allowance: `Ext` has no struct_def in this module, so its field list
// is unknown here and the selection is rendered unchanged.
// POS:      fn opaque_base(v0: &mut Ext) -> u8 {
// POS-NEXT:     v0.whatever
// POS-NEXT: }
emitrust.func @opaque_base(%arg0: !emitrust.mut_ref<!emitrust.struct<"Ext">>) -> ui8 {
  %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Ext">>) -> !emitrust.lvalue<!emitrust.struct<"Ext">>
  %1 = emitrust.member %0["whatever"] : (!emitrust.lvalue<!emitrust.struct<"Ext">>) -> !emitrust.lvalue<ui8>
  %2 = emitrust.load %1 : (!emitrust.lvalue<ui8>) -> ui8
  emitrust.return %2 : ui8
}

// -----

// The defect: the FR-78 marker sits on `Blob`, and "opaque" IS a field of
// `Blob` -- but the selection is made on `S`, which has fields
// ["cmd", "u"]. The marker check never looks at `S`, so nothing caught it.
// CHECK: error: member 'opaque' does not exist on struct 'S'
emitrust.struct_def @S ["cmd", "u"] [ui64, !emitrust.struct<"Blob">]
emitrust.struct_def @Blob ["opaque"] [!emitrust.array<8xui8>] {emitrust.opaque_union}
emitrust.func @leak_through_parent() -> ui8 {
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"S">>
  %1 = emitrust.member %0["opaque"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.array<8xui8>>
  %c0 = emitrust.constant <0 : index> : index
  %2 = emitrust.subscript %1[%c0] : (!emitrust.lvalue<!emitrust.array<8xui8>>, index) -> !emitrust.lvalue<ui8>
  %3 = emitrust.load %2 : (!emitrust.lvalue<ui8>) -> ui8
  emitrust.return %3 : ui8
}

// -----

// The same class with no union anywhere in sight: a plain record and a
// field name it does not have. Nothing marker-shaped is involved, which is
// the point -- the backstop is structural, not marker-driven.
// CHECK: error: member 'missing' does not exist on struct 'Plain'
emitrust.struct_def @Plain ["a", "b"] [i32, i32]
emitrust.func @leak_plain(%arg0: !emitrust.mut_ref<!emitrust.struct<"Plain">>) -> i32 {
  %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Plain">>) -> !emitrust.lvalue<!emitrust.struct<"Plain">>
  %1 = emitrust.member %0["missing"] : (!emitrust.lvalue<!emitrust.struct<"Plain">>) -> !emitrust.lvalue<i32>
  %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %2 : i32
}

// -----

// ORDERING, the load-bearing half: an ARM selection on the marked union
// `U` is both a marker violation and a structural one, and FR-78's wording
// -- the one errors.mlir pins -- must still be the one that fires. The
// `--implicit-check-not` above forbids the structural wording for 'U'
// anywhere in this file's output.
// CHECK: error: opaque union 'U' member access leaked to emission; the importer must reject this at the access site
emitrust.struct_def @U ["opaque"] [!emitrust.array<8xui8>] {emitrust.opaque_union}
emitrust.struct_def @Rec ["u"] [!emitrust.struct<"U">]
emitrust.func @leak_arm(%arg0: !emitrust.mut_ref<!emitrust.struct<"Rec">>) -> i32 {
  %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Rec">>) -> !emitrust.lvalue<!emitrust.struct<"Rec">>
  %1 = emitrust.member %0["u"] : (!emitrust.lvalue<!emitrust.struct<"Rec">>) -> !emitrust.lvalue<!emitrust.struct<"U">>
  %2 = emitrust.member %1["a"] : (!emitrust.lvalue<!emitrust.struct<"U">>) -> !emitrust.lvalue<i32>
  %3 = emitrust.load %2 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %3 : i32
}

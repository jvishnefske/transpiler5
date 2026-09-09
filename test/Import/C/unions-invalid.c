// B1 negative space: the one-slot union model admits only arms that alias
// the first arm's leaf exactly (identical mapped type) or bit-exactly
// (same-width scalars: int arms differing only in signedness, and a float
// paired with a same-width integer).
//
// FR-167 PHASE 2 MOVED THIS FRONTIER FORWARD, and five of this file's
// eight pins move WITH it rather than loosening. `collectUnionSlot` now
// runs the one-slot collection as a TRIAL and falls back to FR-78's
// sizeof-sized opaque byte blob where it used to `return failure()`, so
// scalar arms of differing sizes (int/short, float/long, double/float),
// an aggregate arm over a scalar slot, and a pointer arm are no longer
// DECLARATION-level rejections at all: they import as
// `["opaque"] [!emitrust.array<Nxui8>]` with `emitrust.opaque_union`, and
// the rejection moves to every ACCESS through an arm (pinned in
// union-opaque-arm-access.c) -- a strictly later, strictly more precise
// place to refuse. Those five pins are therefore now EXACT positive
// shape checks, including the blob's C `sizeof`; a regression that
// re-rejected any of them, or that emitted a differently-sized blob,
// fails here.
//
// WHAT REMAINS NEGATIVE, and is INELIGIBLE for the blob by construction:
// a BIT-FIELD arm (not addressable storage; its rejection points at the
// ARM and that location must survive) and an EMPTY union (no arms to
// stand in for, no bytes to alias). Both keep the "unsupported: union"
// stem pin so GREEN may sharpen the wording without loosening the
// location or the union mention. The trailing access-level pin (++ on a
// float pun arm) keeps its own general wording, and its union is a
// same-width pun the one-slot model still takes -- proof the trial does
// not perturb a succeeding path.
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/size-mismatch.c | FileCheck %s --check-prefix=SIZE
// RUN: emitrust-import-c %t/float-size.c | FileCheck %s --check-prefix=FSIZE
// RUN: emitrust-import-c %t/double-float.c | FileCheck %s --check-prefix=DFLOAT
// RUN: emitrust-import-c %t/aggregate-arm.c | FileCheck %s --check-prefix=AGG
// RUN: emitrust-import-c %t/pointer-arm.c | FileCheck %s --check-prefix=PTR
// RUN: not emitrust-import-c %t/bitfield-arm.c 2>&1 | FileCheck %s --check-prefix=BITFIELD
// RUN: not emitrust-import-c %t/empty.c 2>&1 | FileCheck %s --check-prefix=EMPTY
// RUN: not emitrust-import-c %t/float-incdec.c 2>&1 | FileCheck %s --check-prefix=FINCDEC

// SIZE: emitrust.struct_def @S ["opaque"] [!emitrust.array<4xui8>] {{.*}}emitrust.opaque_union}
// FSIZE: emitrust.struct_def @F ["opaque"] [!emitrust.array<8xui8>] {{.*}}emitrust.opaque_union}
// DFLOAT: emitrust.struct_def @D ["opaque"] [!emitrust.array<8xui8>] {{.*}}emitrust.opaque_union}
// AGG: emitrust.struct_def @Wrap ["v"] [i32]
// AGG: emitrust.struct_def @A ["opaque"] [!emitrust.array<4xui8>] {{.*}}emitrust.opaque_union}
// PTR: emitrust.struct_def @P ["opaque"] [!emitrust.array<8xui8>] {{.*}}emitrust.opaque_union}
// BITFIELD: bitfield-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// BITFIELD-NOT: opaque
// EMPTY: empty.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// EMPTY-NOT: opaque
// FINCDEC: float-incdec.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: ++/-- on a non-integer operand

//--- size-mismatch.c
// Same int family but different widths: the arms do not share one leaf,
// so the one-slot trial fails and the 4-byte blob takes over.
union S {
  int i;
  short s;
};

union S g;

//--- float-size.c
// A float arm only aliases a SAME-WIDTH integer slot; float against a
// 64-bit integer is a width mismatch, not a pun -- so the blob takes it.
union F {
  long l;
  float f;
};

union F g;

//--- double-float.c
// Two float types of different widths share no object representation the
// one-slot model can carry. The blob carries the bytes; float leaves are
// outside FR-83's integer byte-view family, so every arm access rejects.
union D {
  double d;
  float f;
};

union D g;

//--- aggregate-arm.c
// An aggregate arm mixed with a scalar arm has no single scalar slot to
// alias (same byte size on this target does not help: the struct is not
// a scalar the reinterpret casts can move). FR-78's `allArmsAggregate`
// predicate missed this shape because the FIRST arm is a record and the
// SECOND is a scalar; phase 2's fallback does not care which is which.
struct Wrap {
  int v;
};

union A {
  struct Wrap w;
  int i;
};

union A g;

//--- pointer-arm.c
// A pointer arm never aliases an integer slot (both arms are 8 bytes
// here, so the one-slot refusal is about the pointer, not the width).
// The blob stands in for the storage; the pointer arm has no
// representation and every access through it rejects at its own site.
union P {
  long l;
  int *p;
};

union P g;

//--- bitfield-arm.c
// A bit-field arm is not addressable storage the slot model can alias --
// and it is INELIGIBLE for the phase 2 blob for the same reason, so this
// union never enters the trial and keeps its untouched wording.
union B {
  int a : 3;
  int b;
};

union B g;

//--- empty.c
// An empty union (GNU extension clang accepts in C) has no first arm and
// therefore no representable slot -- and no arms for a blob to stand in
// for either, so it is INELIGIBLE and keeps its untouched wording.
union E {
};

union E g;

//--- float-incdec.c
// ++/-- through a float pun arm reinterprets the loaded slot value to
// the arm's own (float) type FIRST, so it lands on the general
// non-integer ++/-- rejection instead of doing raw integer arithmetic on
// the bit pattern (the importer rejects float ++/-- everywhere).
union FPun {
  unsigned int u;
  float f;
};

int main(void) {
  union FPun p;
  p.u = 0;
  p.f++;
  return 0;
}

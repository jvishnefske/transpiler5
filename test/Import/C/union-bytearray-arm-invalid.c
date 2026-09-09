// CTS-F (00210) negative space: byte-array arm admission is a TYPE-level
// concession only — the array arm may exist so the 00210 typedefs import,
// but no access ever flows through it, and only an equal-total-width
// integer-array arm qualifies for the ONE-SLOT model.
//
// FR-167 PHASE 2 SPLITS THIS FILE'S TWO PINS APART, and the split is the
// point. The ACCESS pin is unchanged and unchangeable: an equal-width
// byte-array arm still takes the one-slot model, so `x.b[0]` still
// rejects at the access site with its own authored wording. The
// UNEQUAL-width pin MOVES FORWARD: an unequal-total-width array arm is no
// longer a DECLARATION rejection -- it is exactly the `return failure()`
// phase 2 replaced with FR-78's sizeof-sized opaque blob -- so it now
// imports, and its arm accesses reject at their own sites instead. The
// pin is now an exact positive shape check including the blob's C
// `sizeof`.
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/arm-access.c 2>&1 | FileCheck %s --check-prefix=ACCESS
// RUN: emitrust-import-c %t/unequal-width.c | FileCheck %s --check-prefix=UNEQUAL

//--- arm-access.c
// Reading (or writing) through the byte-array arm is rejected AT THE
// ACCESS SITE with its own wording (authored here, binding): the type
// admitted, but x.b[0] cannot be modeled on the one integer slot.
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

union U16 {
  uint16_t u;
  uint8_t b[2];
};

int main(void) {
  union U16 x;
  x.u = 42;
  return x.b[0];
}
// ACCESS: arm-access.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union byte-array arm access

//--- unequal-width.c
// An array arm whose total width differs from the integer arm's does not
// share the slot, so the one-slot trial fails -- and phase 2's blob
// stands in for the 4 bytes C gives this union (u16 arm, 3-byte u8 arm,
// align 2). Both arms are access-rejected; only the DECLARATION moved.
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

union Wide {
  uint16_t u;
  uint8_t b[3];
};

union Wide g;
// UNEQUAL: emitrust.struct_def @Wide ["opaque"] [!emitrust.array<4xui8>] {{.*}}emitrust.opaque_union}

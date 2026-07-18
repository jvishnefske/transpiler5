// CTS-F (00210) negative space: byte-array arm admission is a TYPE-level
// concession only — the array arm may exist so the 00210 typedefs import,
// but no access ever flows through it, and only an equal-total-width
// integer-array arm qualifies.
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/arm-access.c 2>&1 | FileCheck %s --check-prefix=ACCESS
// RUN: not emitrust-import-c %t/unequal-width.c 2>&1 | FileCheck %s --check-prefix=UNEQUAL

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
// share the slot and keeps the existing union family rejection (stem pin,
// matching unions-invalid.c: GREEN may sharpen the wording but not the
// location or the union mention).
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

union Wide {
  uint16_t u;
  uint8_t b[3];
};

union Wide g;
// UNEQUAL: unequal-width.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union

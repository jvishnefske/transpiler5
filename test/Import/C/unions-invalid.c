// B1 negative space: the one-slot union model admits only arms that alias
// the first arm's leaf exactly (identical mapped type, or same-width int
// arms differing only in signedness). Everything else stays rejected with
// a located union diagnostic: a float arm mixed with int, arms of
// differing sizes, a pointer arm, a bit-field arm, and an empty union
// (which has no representable slot at all). Each pin requires only the
// "unsupported: union" stem so GREEN may sharpen the wording per case
// without loosening the location or the union mention.
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/float-arm.c 2>&1 | FileCheck %s --check-prefix=FLOAT
// RUN: not emitrust-import-c %t/size-mismatch.c 2>&1 | FileCheck %s --check-prefix=SIZE
// RUN: not emitrust-import-c %t/pointer-arm.c 2>&1 | FileCheck %s --check-prefix=PTR
// RUN: not emitrust-import-c %t/bitfield-arm.c 2>&1 | FileCheck %s --check-prefix=BITFIELD
// RUN: not emitrust-import-c %t/empty.c 2>&1 | FileCheck %s --check-prefix=EMPTY

// FLOAT: float-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// SIZE: size-mismatch.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// PTR: pointer-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// BITFIELD: bitfield-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// EMPTY: empty.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union

//--- float-arm.c
// int/float punning cannot be modeled by a signedness cast on one slot.
union F {
  int i;
  float f;
};

union F g;

//--- size-mismatch.c
// Same int family but different widths: the arms do not share one leaf.
union S {
  int i;
  short s;
};

union S g;

//--- pointer-arm.c
// A pointer arm never aliases an integer slot (both arms are 8 bytes
// here, so the rejection is about the pointer, not the width).
union P {
  long l;
  int *p;
};

union P g;

//--- bitfield-arm.c
// A bit-field arm is not addressable storage the slot model can alias.
union B {
  int a : 3;
  int b;
};

union B g;

//--- empty.c
// An empty union (GNU extension clang accepts in C) has no first arm and
// therefore no representable slot.
union E {
};

union E g;

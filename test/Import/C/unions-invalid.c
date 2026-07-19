// B1 negative space: the one-slot union model admits only arms that alias
// the first arm's leaf exactly (identical mapped type) or bit-exactly
// (same-width scalars: int arms differing only in signedness, and a float
// paired with a same-width integer). Everything else stays rejected with
// a located union diagnostic: scalar arms of differing sizes (int/int,
// float/int, and double/float), an aggregate arm, a pointer arm, a
// bit-field arm, and an empty union (which has no representable slot at
// all). Each union-shape pin requires only the "unsupported: union" stem
// so GREEN may sharpen the wording per case without loosening the
// location or the union mention; the trailing access-level pin (++ on a
// float pun arm) keeps its own general wording.
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/size-mismatch.c 2>&1 | FileCheck %s --check-prefix=SIZE
// RUN: not emitrust-import-c %t/float-size.c 2>&1 | FileCheck %s --check-prefix=FSIZE
// RUN: not emitrust-import-c %t/double-float.c 2>&1 | FileCheck %s --check-prefix=DFLOAT
// RUN: not emitrust-import-c %t/aggregate-arm.c 2>&1 | FileCheck %s --check-prefix=AGG
// RUN: not emitrust-import-c %t/pointer-arm.c 2>&1 | FileCheck %s --check-prefix=PTR
// RUN: not emitrust-import-c %t/bitfield-arm.c 2>&1 | FileCheck %s --check-prefix=BITFIELD
// RUN: not emitrust-import-c %t/empty.c 2>&1 | FileCheck %s --check-prefix=EMPTY
// RUN: not emitrust-import-c %t/float-incdec.c 2>&1 | FileCheck %s --check-prefix=FINCDEC

// SIZE: size-mismatch.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// FSIZE: float-size.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// DFLOAT: double-float.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// AGG: aggregate-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// PTR: pointer-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// BITFIELD: bitfield-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// EMPTY: empty.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// FINCDEC: float-incdec.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: ++/-- on a non-integer operand

//--- size-mismatch.c
// Same int family but different widths: the arms do not share one leaf.
union S {
  int i;
  short s;
};

union S g;

//--- float-size.c
// A float arm only aliases a SAME-WIDTH integer slot; float against a
// 64-bit integer is a width mismatch, not a pun.
union F {
  long l;
  float f;
};

union F g;

//--- double-float.c
// Two float types of different widths share no object representation the
// one-slot model can carry.
union D {
  double d;
  float f;
};

union D g;

//--- aggregate-arm.c
// An aggregate arm mixed with a scalar arm has no single scalar slot to
// alias (same byte size on this target does not help: the struct is not
// a scalar the reinterpret casts can move).
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

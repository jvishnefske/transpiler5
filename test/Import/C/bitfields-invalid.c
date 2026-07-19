// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/union-arm.c 2>&1 | FileCheck %s --check-prefix=UNIONARM
// RUN: not emitrust-import-c %t/sizeof-bitfields.c 2>&1 | FileCheck %s --check-prefix=SIZEOF

// C99-45 out-of-scope pins for the bit-field accessor model.
//
// A bit-field arm in a union stays rejected: a bit-field is not
// addressable storage the one-slot union model can alias (unchanged
// wording from the union import rules).
//
// sizeof of a struct containing bit-fields is rejected conservatively:
// the backing-run model gives the struct a well-defined Rust size, but
// that size need not match the C ABI layout sizeof would promise, so the
// importer refuses rather than asserting ABI-exact layout. (Taking the
// address of a bit-field is a clang error and needs no pin here.)

// UNIONARM: union-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union with a bit-field arm
// SIZEOF: sizeof-bitfields.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: sizeof of a struct with bit-fields

//--- union-arm.c
union U {
  int a;
  unsigned b : 3;
};

int get(union U u) {
  return u.a;
}

//--- sizeof-bitfields.c
struct P {
  unsigned a : 3;
  unsigned b : 5;
};

int size_of(void) {
  return sizeof(struct P);
}

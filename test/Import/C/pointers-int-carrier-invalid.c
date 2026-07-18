// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/deref.c 2>&1 | FileCheck %s --check-prefix=DEREF
// RUN: not emitrust-import-c %t/arith.c 2>&1 | FileCheck %s --check-prefix=ARITH
// RUN: not emitrust-import-c %t/mixed.c 2>&1 | FileCheck %s --check-prefix=MIXED

// Int-carrier boundaries (see pointers-int-carrier.c). A carrier is an
// integer in pointer clothing: it has no base object, so using it AS a
// pointer — dereference, element arithmetic — has nothing to resolve
// against and stays a located rejection. A region that sees both a
// carrier source and a real address base straddles the two models and
// stays rejected as well.

// Dereferencing a carrier would read from a fabricated address.
// DEREF: deref.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: dereference of an integer-carrier pointer

//--- deref.c
typedef unsigned long size_t;
int deref(size_t v) {
  int *p = (int *)v;
  return *p;
}

// Pointer arithmetic on a carrier has no element run to walk.
// ARITH: arith.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer arithmetic on an integer-carrier pointer

//--- arith.c
typedef unsigned long size_t;
int arith(size_t v) {
  char *p = (char *)v;
  p = p + 1;
  return 0;
}

// A pointer with a real address base cannot also absorb an
// integer-to-pointer cast: the write is a non-address value for the
// based region (the pre-carrier rejection wording is kept).
// MIXED: mixed.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- mixed.c
typedef unsigned long size_t;
int mixed(size_t v) {
  int x = 3;
  int *p = &x;
  p = (int *)v;
  return x;
}

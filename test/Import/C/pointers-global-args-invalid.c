// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/mixed-class.c 2>&1 | FileCheck %s --check-prefix=MIXED
// RUN: not emitrust-import-c %t/nullable-global.c 2>&1 | FileCheck %s --check-prefix=NULLG

// Boundaries of the cell-slice lowering (CTS-P10). A pointer-parameter
// class lowers to `&[Cell<T>]` only when EVERY base in its
// interprocedural class is a mutable global of one element type. Mixing
// a global base with a local base would need one parameter type that is
// both a plain `&mut [T]` (locals have no Cells) and a `&[Cell<T>]`, so
// the mixed class stays rejected in v1. A parameter that is compared
// against null (nullable) cannot be global-backed either: the Option
// wrapping and the global_cells borrow discipline do not compose in v1.

// The same function is called once with a global array and once with a
// local array: the parameter's class would join both bases.
// MIXED: mixed-class.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer parameter would join global 'G' and local object 'larr' into one region

//--- mixed-class.c
int G[4];

int first(int *a) {
  return a[0] + a[1];
}

int main(void) {
  int larr[4];
  larr[0] = 2;
  larr[1] = 3;
  int x = first(G);
  int y = first(larr);
  return x + y;
}

// A global-backed parameter that is compared against NULL in some path
// is nullable; nullable cell-slices stay rejected in v1.
// NULLG: nullable-global.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: nullable pointer parameter backed by a global variable

//--- nullable-global.c
int G[4];

int maybe(int *a) {
  if (a == 0)
    return -1;
  return a[1];
}

int main(void) {
  return maybe(G);
}

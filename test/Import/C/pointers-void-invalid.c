// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/short-view.c 2>&1 | FileCheck %s --check-prefix=SHORT
// RUN: not emitrust-import-c %t/float-view.c 2>&1 | FileCheck %s --check-prefix=FLOAT
// RUN: not emitrust-import-c %t/void-param.c 2>&1 | FileCheck %s --check-prefix=VOIDPARAM
// RUN: not emitrust-import-c %t/void-deref.c 2>&1 | FileCheck %s --check-prefix=VOIDDEREF

// CTS-P9 boundaries of the pointee-wildcard `void *`: a reinterpret-back
// site `*(T *)p` must type-check T against the region's base element
// type. Exact matches and same-width int<->int views are the whole
// positive space (pointers-void.c); every other reinterpretation, a
// `void *` crossing a function boundary, and a deref that never names an
// element type stay located rejections.

// A different-width integer view over an int base would split the base
// element; there is no cast that models it.
// SHORT: short-view.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer cast reinterprets the pointee ('short' over 'int' storage)

//--- short-view.c
int main(void) {
  int x = 65535;
  void *p = &x;
  return *(short *)p;
}

// A float view over an int base is a bit-pattern reinterpretation, not a
// value conversion; `as` cannot model it.
// FLOAT: float-view.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer cast reinterprets the pointee ('float' over 'int' storage)

//--- float-view.c
int main(void) {
  int x = 3;
  void *p = &x;
  float f = *(float *)p;
  return f > 0.0f;
}

// A `void *` parameter has no element type to classify against and no
// region to join at the call boundary.
// VOIDPARAM: void-param.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: void pointer parameter

//--- void-param.c
int deref(void *p) { return *(int *)p; }
int main(void) {
  int x = 1;
  return deref(&x);
}

// Dereferencing a `void *` without a reinterpret-back cast (a GNU
// extension in C) never names an element type at all.
// VOIDDEREF: void-deref.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: dereference of a 'void *' pointer

//--- void-deref.c
int main(void) {
  int x = 3;
  void *p = &x;
  *p;
  return 0;
}

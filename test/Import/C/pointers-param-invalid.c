// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/aliasing.c 2>&1 | FileCheck %s --check-prefix=ALIAS
// RUN: not emitrust-import-c %t/scalar-to-slice.c 2>&1 | FileCheck %s --check-prefix=SCALAR
// RUN: not emitrust-import-c %t/cross-tu-decl.c %S/Inputs/pointer-param-shape.c 2>&1 | FileCheck %s --check-prefix=CROSSTU

// Phase-1b pointer-parameter boundaries: call-site borrows must not alias,
// a slice parameter needs a whole element run behind the argument, and a
// cross-TU prototype imported before the defining TU must not have been
// called against the unrefined scalar-reference shape.

// Two borrow-producing arguments resolving to the same region base would
// be two simultaneous &mut borrows of one object in Rust. The array is
// kept above the 32-element owner-promotion limit: a smaller array would
// legally absorb this shape as a Phase-4 owner method, whose i64 index
// parameters carry no borrows at all (see owners.c).
// ALIAS: aliasing.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 'arr')

//--- aliasing.c
void pair(int *a, int *b) {
  a[0] = b[1];
}

int main(void) {
  int arr[64];
  arr[0] = 1;
  pair(arr, &arr[2]);
  return arr[0];
}

// The address of a scalar carries no element run to reslice; only whole
// arrays (or other slices) can feed a subscripted parameter.
// SCALAR: scalar-to-slice.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the address of a scalar object cannot be passed as a slice parameter

//--- scalar-to-slice.c
int first(int *a) {
  return a[0] + a[1];
}

int main(void) {
  int x = 3;
  return first(&x);
}

// Cross-TU shape mismatch: this TU sees only the prototype, classifies the
// pointer parameter as a scalar reference, and imports a call against that
// shape; the companion TU's definition subscripts the parameter and would
// refine the signature to a slice, which the already-imported call cannot
// follow. The diagnostic names the function and both shapes.
// CROSSTU: pointer-param-shape.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function 'tally' was called as '(!emitrust.mut_ref<i32>, i32) -> i32' before its definition refined the signature to '(!emitrust.mut_ref<!emitrust.slice<i32>>, i32) -> i32' (cross-TU pointer-parameter classification)

//--- cross-tu-decl.c
int tally(int *a, int n);

int main(void) {
  int arr[3];
  arr[0] = 1;
  arr[1] = 2;
  arr[2] = 3;
  return tally(arr, 3);
}

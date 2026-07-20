// W3.1 multi-TU gate oracle (G3 NEGATIVE): `fill` is called on ONE local
// array (`a` in `main`, this TU) and ALSO called (see the companion) on a
// SECOND, unrelated local array (`b`) from a different function in
// another TU. Even after W3.2 hoists call-site visibility to a
// whole-program pre-pass, the interprocedural class `fill`'s parameter
// belongs to would then unify TWO distinct local bases across the two
// TUs — the existing single-TU multi-base rule (see
// test/Import/C/owners-fallback.c's TWOARR case) already keeps a
// multi-base class on the plain slice lowering, and that rule is
// completely orthogonal to the visibility gate this wave is about: it
// must keep firing once the hoist makes both call sites visible
// together. This test pins TODAY's (visibility-gate-caused) fallback;
// after W3.2 the SAME fallback must persist for the multi-base reason
// instead.
// RUN: emitrust-import-c %s %S/Inputs/multi-tu-gate-g3-owner-multibase-other.c | FileCheck %s

int fill(int *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    p[i] = i;
  return p[0];
}

int main(void) {
  int a[4];
  return fill(a, 4);
}

// CHECK-LABEL: func.func @fill
// CHECK-SAME: (%{{.*}}: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.*}}: i32) -> i32
// CHECK-LABEL: func.func @c_main
// CHECK: emitrust.slice_of mut
// CHECK: call @fill(
// CHECK-NOT: emitrust.struct_def @Owner_
// CHECK-NOT: emitrust.method_of
// CHECK-NOT: emitrust.method_call

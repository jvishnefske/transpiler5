// Phase-4 owner lift x the hosted byte-family lowering: which images the
// memcpy/memmove call sites INSIDE a promoted owner method take, and why.
//
// `planOwners` promotes a class all-or-nothing: every data-pointer parameter
// of every method resolves into ONE storage base, so inside the method every
// such parameter is an i64 element index into the SINGLE receiver region
// `self.data`. Two of them therefore designate cursors into one whole region
// of known extent even though their C roots are DISTINCT parameter decls,
// which the (root, field-path) aliasing key cannot see. Before this test the
// pair emitted `&mut self.data[dst..]` next to `&self.data[src..]` -- rustc
// E0502, a crate that does not build, with emitrust-cc still exiting 0 (the
// FR-140/141/142/146 silent-unbuildable class).
//
// The region is PROVABLE here -- it is the owner's own array, of known
// extent, indexed by absolute cursors -- which is exactly the condition the
// same-root local-array branch already rides `copy_within` on, so the pair
// joins that image (memmove's overlap-correct semantics, refining C's
// undefined OVERLAPPING memcpy; the naive two-borrow form is not merely
// wrong here, it does not compile). The runtime oracle is
// test/EndToEnd/owner-region-memcpy-alias.c: this file only pins the image.
//
// One helper per array on purpose: a helper shared between two arrays would
// unify their storage bases and the class would not promote at all.
// RUN: emitrust-import-c %s | FileCheck %s

#include <string.h>

// Two pointer parameters of ONE promoted region: the whole receiver array is
// borrowed mutably once and the helper takes both absolute cursors.
static void mv(char *dst, const char *src, int n) {
  memmove(dst, src, (size_t)n);
}
// CHECK-LABEL: func.func @mv
// CHECK-NOT: emitrust.call_opaque "__emitrust_memcpy"
// CHECK: emitrust.call_opaque "__emitrust_memcpy_within"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<i8>>, i64, i64, i64) -> ()

// The same shape with memcpy and with an unsigned char region takes the u8
// image of the same helper.
static void cpu(unsigned char *dst, const unsigned char *src, int n) {
  memcpy(dst, src, (size_t)n);
}
// CHECK-LABEL: func.func @cpu
// CHECK-NOT: emitrust.call_opaque "__emitrust_memcpy_u8"
// CHECK: emitrust.call_opaque "__emitrust_memcpy_within_u8"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, i64, i64, i64) -> ()

// A method-LOCAL array is a second, disjoint storage place, so a copy out of
// it keeps the ordinary two-slice image: nothing here aliases.
static void fill(char *dst, int n) {
  char t[4] = {1, 2, 3, 4};
  memcpy(dst, t, (size_t)n);
}
// CHECK-LABEL: func.func @fill
// CHECK-NOT: emitrust.call_opaque "__emitrust_memcpy_within"
// CHECK: emitrust.call_opaque "__emitrust_memcpy"(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<i8>>, !emitrust.ref<!emitrust.slice<i8>>, i64) -> ()

// Two cursors into two DIFFERENT arrays: the parameters unify TWO storage
// bases, the class never promotes, and both arguments stay independent slice
// borrows of independent regions. This must keep working unchanged.
static void cx(char *dst, const char *src, int n) {
  memcpy(dst, src, (size_t)n);
}
// CHECK-LABEL: func.func @cx
// CHECK-NOT: emitrust.call_opaque "__emitrust_memcpy_within"
// CHECK: emitrust.call_opaque "__emitrust_memcpy"(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<i8>>, !emitrust.ref<!emitrust.slice<i8>>, i64) -> ()

int main(void) {
  char a[8];
  unsigned char u[8];
  char g[8];
  char p[8];
  char q[8];
  memset(a, 0, 8);
  memset(u, 0, 8);
  memset(g, 0, 8);
  memset(p, 0, 8);
  memset(q, 0, 8);
  mv(&a[2], &a[0], 4);
  cpu(&u[4], &u[0], 4);
  fill(&g[2], 4);
  cx(&p[0], &q[2], 3);
  return a[0] + u[0] + g[0] + p[0];
}

// CHECK: emitrust.verbatim "fn __emitrust_memcpy_within(s: &mut [i8], dst: i64, src: i64, n: i64)
// CHECK: emitrust.verbatim "fn __emitrust_memcpy_within_u8(s: &mut [u8], dst: i64, src: i64, n: i64)

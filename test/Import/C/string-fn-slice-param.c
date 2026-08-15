// RUN: emitrust-import-c %s | FileCheck %s

// FR-72: hosted byte-family <string.h> calls (memset/memcpy/memmove/
// memcmp) accept byte-slice PARAMETERS as their regions, not only char
// ARRAYS: a pointer argument resolving to a slice-kind parameter
// (`!emitrust.mut_ref/ref<!emitrust.slice<i8|ui8>>`, the existing
// cursor-param convention) reslices the parameter's deref'd backing at
// the argument's cursor — `emitrust.slice_of` over the
// `!emitrust.lvalue<!emitrust.slice<...>>` place, no new ops. This pins
// the tinycrypt `_set` shape (a `void *` parameter whose ONLY use is the
// memset destination, admitted by the FR-71 scan's new byte-family-call
// arm with the ui8 element), memset at a NONZERO cursor and memcpy over
// `uint8_t *` parameters, memcmp over a const (shared) source pair, and
// the ELEMENT AGREEMENT rule: ui8 regions route to the parallel
// `__emitrust_*_u8` helper images while an i8 (char) region keeps the
// existing i8 helpers — the emitted helper signature always matches the
// call site, so no crate can fail element typing only at rustc. The
// char-ARRAY path is byte-for-byte untouched (strings-hosted.c).

#include <string.h>
typedef unsigned char uint8_t;

// The tinycrypt `_set` shape: the void* param's ONLY use is the memset
// destination — no byte-pointer local, no cast — so the FR-71 scan's
// byte-family-call arm admits it with the ui8 element.
void _set(void *to, uint8_t val, unsigned len) {
  memset(to, val, len);
}

// CHECK-LABEL: func.func @_set
// CHECK-SAME: (%arg0: !emitrust.mut_ref<!emitrust.slice<ui8>>, %arg1: ui8, %arg2: ui32)
// CHECK: %[[SETP:.*]] = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.slice<ui8>>) -> !emitrust.lvalue<!emitrust.slice<ui8>>
// CHECK: %[[SETD:.*]] = emitrust.slice_of mut %[[SETP]][%{{.*}}] : (!emitrust.lvalue<!emitrust.slice<ui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK: emitrust.call_opaque "__emitrust_memset_u8"(%[[SETD]], %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, i32, i64) -> ()

// A `uint8_t *` parameter (already ParamKind::Slice) as the destination
// at a NONZERO cursor: the reslice starts at the pointer expression's
// cursor, exactly like an `&arr[1]` argument over an array.
void fill_bytes(uint8_t *dst, int val, unsigned len) {
  memset(dst + 1, val, len);
}

// CHECK-LABEL: func.func @fill_bytes
// CHECK-SAME: (%arg0: !emitrust.mut_ref<!emitrust.slice<ui8>>, %arg1: i32, %arg2: ui32)
// CHECK: %[[FILLP:.*]] = emitrust.deref %arg0
// CHECK: %[[ONE:.*]] = arith.addi %{{.*}} : i64
// CHECK: %[[FILLD:.*]] = emitrust.slice_of mut %[[FILLP]][%[[ONE]]] : (!emitrust.lvalue<!emitrust.slice<ui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK: emitrust.call_opaque "__emitrust_memset_u8"(%[[FILLD]], %{{.*}}, %{{.*}})

// memcpy between two DISTINCT byte-slice parameters: mutable destination
// reslice, shared source reslice (the const pointee borrows shared), u8
// helper image. Distinct parameters are assumed non-overlapping exactly
// as the cursor convention already assumes.
void copy_bytes(uint8_t *dst, const uint8_t *src, unsigned len) {
  memcpy(dst, src, len);
}

// CHECK-LABEL: func.func @copy_bytes
// CHECK-SAME: (%arg0: !emitrust.mut_ref<!emitrust.slice<ui8>>, %arg1: !emitrust.ref<!emitrust.slice<ui8>>, %arg2: ui32)
// CHECK: emitrust.slice_of mut %{{.*}} -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK: emitrust.slice_of %{{.*}} -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: emitrust.call_opaque "__emitrust_memcpy_u8"(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, !emitrust.ref<!emitrust.slice<ui8>>, i64) -> ()

// memcmp over a const (shared) parameter pair: both reslices are shared
// borrows and the value-position result is C's int.
int cmp_bytes(const uint8_t *a, const uint8_t *b, unsigned len) {
  return memcmp(a, b, len);
}

// CHECK-LABEL: func.func @cmp_bytes
// CHECK-SAME: (%arg0: !emitrust.ref<!emitrust.slice<ui8>>, %arg1: !emitrust.ref<!emitrust.slice<ui8>>, %arg2: ui32) -> i32
// CHECK: emitrust.slice_of %{{.*}} -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: emitrust.slice_of %{{.*}} -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: emitrust.call_opaque "__emitrust_memcmp_u8"(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.slice<ui8>>, !emitrust.ref<!emitrust.slice<ui8>>, i64) -> i32

// ELEMENT CARE: a `char *` slice parameter is an i8 region and keeps the
// EXISTING i8 helper — the u8 image is selected by the region element,
// never blanket-substituted.
void cset(char *d, int c, unsigned n) {
  memset(d, c, n);
}

// CHECK-LABEL: func.func @cset
// CHECK-SAME: (%arg0: !emitrust.mut_ref<!emitrust.slice<i8>>, %arg1: i32, %arg2: ui32)
// CHECK: emitrust.slice_of mut %{{.*}} -> !emitrust.mut_ref<!emitrust.slice<i8>>
// CHECK: emitrust.call_opaque "__emitrust_memset"(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<i8>>, i32, i64) -> ()

// Each function is called with TWO distinct arrays so the FR-40 owner
// lift (which would fold a unique array into a method receiver) stays
// out of the way: this test pins the plain slice-parameter convention.
int main(void) {
  uint8_t a[8];
  uint8_t b[8];
  char t[4];
  char u[4];
  _set(a, 1, 8u);
  _set(b, 2, 8u);
  fill_bytes(a, 2, 4u);
  fill_bytes(b, 3, 4u);
  copy_bytes(b, a, 8u);
  copy_bytes(a, b, 8u);
  cset(t, 'x', 3u);
  cset(u, 'y', 2u);
  return cmp_bytes(a, b, 8u) + t[0] + u[0];
}

// Call sites ride the existing slice-argument convention: byte arrays
// reslice from cursor 0 with the element matched against the parameter.
// CHECK-LABEL: func.func @c_main
// CHECK: emitrust.slice_of mut %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK: call @_set(
// CHECK: call @fill_bytes(
// CHECK: call @copy_bytes(
// CHECK: emitrust.slice_of mut %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// CHECK: call @cset(
// CHECK: call @cmp_bytes(

// Each requested helper is emitted exactly once, as safe Rust; the u8
// images live beside the i8 originals and agree with them byte-wise.
// CHECK: emitrust.verbatim "fn __emitrust_memset(s: &mut [i8], c: i32, n: i64)
// CHECK: emitrust.verbatim "fn __emitrust_memset_u8(s: &mut [u8], c: i32, n: i64)
// CHECK: emitrust.verbatim "fn __emitrust_memcpy_u8(dst: &mut [u8], src: &[u8], n: i64)
// CHECK: emitrust.verbatim "fn __emitrust_memcmp_u8(a: &[u8], b: &[u8], n: i64) -> i32
// CHECK-NOT: unsafe

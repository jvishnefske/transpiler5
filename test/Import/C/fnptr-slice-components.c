// FR-76: fn-ptr COMPONENTS through the parameter mapper. Before this, a
// function-pointer type whose signature names a data pointer
// (`void (*cb)(uint8_t *buf, unsigned len)`) funneled its components
// through the plain type mapper into the pointer residual and rejected
// the WHOLE record — 47 of 80 measured cascade roots. This pins the new
// contract: an arithmetic-pointee scalar-pointer component classifies as
// a region-typed slice exactly like a body-classified Slice parameter
// (`!emitrust.mut_ref<!emitrust.slice<T>>`, shared `&[u8]` for the
// const-u8 flavor per CTS-BR/FR-55), so the field, the local, the
// `Some(f)` binding, and the call through the field all import. The
// unification side (option (b) of the FR-76 spike): an ADDRESS-TAKEN
// function's own arithmetic-pointee ScalarRef parameters are forced to
// the same Slice classification — even a deref-only body (`*buf = ...`,
// which collectSliceParams alone would keep ScalarRef) — and an
// address-taken function is disqualified from Phase-4 owner promotion (a
// fn-pointer call site cannot thread a receiver), so the natural
// callback-table shape (main's local buffer through the field AND
// through a direct call) binds with exactly equal signatures. Call sites
// through the fn-ptr pass region views (`emitrust.slice_of`) exactly
// like a direct call to a slice-classified callee.
//
// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | FileCheck --check-prefix=DEF %s

#include <stdint.h>

struct Ops {
  void (*cb)(uint8_t *buf, unsigned len);
  int (*sum)(const uint8_t *buf, unsigned len);
};
// DEF: emitrust.struct_def @Ops ["cb", "sum"] [!emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.slice<ui8>>, ui32)>, !emitrust.fn_ptr<(!emitrust.ref<!emitrust.slice<ui8>>, ui32) -> i32>]

// Loop body: Slice by the body classification already; address-taken and
// directly called with main's LOCAL array, so without the FR-76 owner
// disqualifier this would promote to an Owner_ method and mismatch.
void fill(uint8_t *buf, unsigned len) {
  for (unsigned i = 0; i < len; i++)
    buf[i] = (uint8_t)(i * 3u + 1u);
}
// CHECK-LABEL: func.func @fill
// CHECK-SAME: !emitrust.mut_ref<!emitrust.slice<ui8>>

// Deref-only body: collectSliceParams alone keeps this ScalarRef
// (`&mut u8`); the address-taken forcing makes it Slice so the binding
// below matches the fn-ptr component signature.
void poke_first(uint8_t *buf, unsigned len) {
  *buf = (uint8_t)(*buf + len);
}
// CHECK-LABEL: func.func @poke_first
// CHECK-SAME: !emitrust.mut_ref<!emitrust.slice<ui8>>

// Const form: `const uint8_t *` classifies as the SHARED byte slice.
int sum_all(const uint8_t *buf, unsigned len) {
  int s = 0;
  for (unsigned i = 0; i < len; i++)
    s += buf[i];
  return s;
}
// CHECK-LABEL: func.func @sum_all
// CHECK-SAME: !emitrust.ref<!emitrust.slice<ui8>>

int main(void) {
  struct Ops ops;
  // Field assignment: the compatible function binds as a `Some(fill)`
  // constant of the fn-ptr-over-slice-refs field type.
  ops.cb = fill;
  ops.sum = sum_all;
  // CHECK-LABEL: func.func @c_main
  // CHECK: emitrust.constant <#emitrust.opaque<"Some(fill)">> : !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.slice<ui8>>, ui32)>
  // CHECK: emitrust.constant <#emitrust.opaque<"Some(sum_all)">> : !emitrust.fn_ptr<(!emitrust.ref<!emitrust.slice<ui8>>, ui32) -> i32>
  uint8_t b[4];
  // Direct call to the address-taken callee: same slice signature, same
  // region-view argument.
  fill(b, 4u);
  // CHECK: emitrust.slice_of mut %{{.*}} : (!emitrust.lvalue<!emitrust.array<4xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
  // CHECK: call @fill(%{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, ui32) -> ()
  // Invocation through the field: the borrow argument is a region view
  // at the array's cursor, exactly like the direct call above.
  ops.cb(b, 4u);
  // CHECK: emitrust.slice_of mut %{{.*}} : (!emitrust.lvalue<!emitrust.array<4xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
  // CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}, %{{.*}}) : (!emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.slice<ui8>>, ui32)>, !emitrust.mut_ref<!emitrust.slice<ui8>>, ui32) -> ()
  // Rebinding the field to the deref-only callback.
  ops.cb = poke_first;
  ops.cb(b, 4u);
  // CHECK: emitrust.constant <#emitrust.opaque<"Some(poke_first)">> : !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.slice<ui8>>, ui32)>
  // CHECK: emitrust.call_indirect
  // A LOCAL of the same fn-ptr type: the position mattered before FR-76
  // (param/local/field all funneled into the same residual), so the
  // local flavor is pinned too.
  void (*held)(uint8_t *, unsigned) = poke_first;
  held(b, 4u);
  // CHECK: emitrust.variable named "held" : !emitrust.lvalue<!emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.slice<ui8>>, ui32)>>
  // CHECK: emitrust.call_indirect
  // Const form through the field: a SHARED region view feeds the call
  // and the i32 result flows out of the call_indirect.
  int s = ops.sum(b, 4u);
  // CHECK: emitrust.slice_of %{{.*}} : (!emitrust.lvalue<!emitrust.array<4xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
  // CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}, %{{.*}}) : (!emitrust.fn_ptr<(!emitrust.ref<!emitrust.slice<ui8>>, ui32) -> i32>, !emitrust.ref<!emitrust.slice<ui8>>, ui32) -> i32
  return s & 1;
}

// RUN: emitrust-import-c %s | FileCheck %s

// FR-88: a `const uint8_t *` parameter that MAY BE NULL — the body
// null-tests it and touches its region ONLY under a proven null guard
// (tinycrypt ctr_prng's `if (personalization) memcpy(...,
// personalization, ...)` and hmac_prng/ccm's `if (p == (uint8_t *) 0)
// return;` shapes) — imports as an Option-wrapped shared byte slice
// (`!emitrust.opaque<"Option<&[u8]>">`), NOT a plain slice: a plain
// slice has no null representation, which stubbed all three ctr_prng
// entry points. This pins the whole convention: the Option signature,
// the null test lowered to a LET-BOUND `is_some`/`is_none` method call
// (the fn-ptr precedent's spelling; the let-bound i1 is what keeps
// clippy::unnecessary_unwrap away), the guarded region use unwrapped AT
// the use site (inside the guard by construction — MethodCallOp is
// non-Pure, so nothing can hoist the panic path out), call sites
// passing `Some(region)` or an inline `None` for C's null constant, the
// ui8 LOCAL-ARRAY byte-family destination the same tinycrypt shape
// needs (previously i8-only), and the DECLINE side: a null-tested param
// whose use is NOT provably guarded (subscript after a cast-null early
// exit) stays a plain slice whose null test FOLDS — sound because its
// null-constant call sites keep their rejection — with the
// `(uint8_t *) 0` cast spelling now recognized as a null pointer
// constant (it used to reject outright).

#include <string.h>
typedef unsigned char uint8_t;

// POSITIVE polarity (ctr_prng tc_ctr_prng_init): `if (0 != p) memcpy`.
// The destination is a ui8 LOCAL array — the FR-88 byte-family
// admission — and the source is the unwrapped nullable param.
int init(uint8_t *out, const uint8_t *personalization, unsigned int plen) {
  uint8_t buf[16] = {0};
  if (0 != personalization) {
    unsigned int len = plen;
    if (len > sizeof buf) {
      len = sizeof buf;
    }
    memcpy(buf, personalization, len);
  }
  out[0] = buf[0];
  return (int)buf[1];
}

// CHECK-LABEL: func.func @init
// CHECK-SAME: (%arg0: !emitrust.mut_ref<!emitrust.slice<ui8>>, %arg1: !emitrust.opaque<"Option<&[u8]>">, %arg2: ui32) -> i32
// The Option value lives in a named lvalue shadow (method-call receiver).
// CHECK: %[[P:.*]] = emitrust.variable named "personalization" : !emitrust.lvalue<!emitrust.opaque<"Option<&[u8]>">>
// CHECK: emitrust.assign %[[P]] = %arg1
// The null test is the Option discriminant — never a folded constant.
// CHECK: %[[SOME:.*]] = emitrust.method_call %[[P]]["is_some"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<&[u8]>">>) -> i1
// The guarded use unwraps AT the use site, then rides the ordinary
// FR-72 deref + reslice image into the u8 memcpy helper; the ui8 local
// array is the mutable destination.
// CHECK: %[[REF:.*]] = emitrust.method_call %[[P]]["unwrap"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<&[u8]>">>) -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: %[[SRCP:.*]] = emitrust.deref %[[REF]] : (!emitrust.ref<!emitrust.slice<ui8>>) -> !emitrust.lvalue<!emitrust.slice<ui8>>
// CHECK: emitrust.slice_of mut %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<16xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK: emitrust.slice_of %[[SRCP]][%{{.*}}] : (!emitrust.lvalue<!emitrust.slice<ui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: emitrust.call_opaque "__emitrust_memcpy_u8"(

// NEGATIVE polarity (hmac_prng/ccm early exit), with the corpus's
// cast-null spelling in a short-circuit `||`: proven non-null AFTER the
// early return, unwrap after the guard.
int chk(const uint8_t *p, unsigned int len) {
  uint8_t tmp[4];
  if (p == (uint8_t *)0 || len < 4u) {
    return -1;
  }
  memcpy(tmp, p, 4);
  return (int)tmp[0] + (int)tmp[3];
}

// CHECK-LABEL: func.func @chk
// CHECK-SAME: (%arg0: !emitrust.opaque<"Option<&[u8]>">, %arg1: ui32) -> i32
// CHECK: %[[CP:.*]] = emitrust.variable named "p" : !emitrust.lvalue<!emitrust.opaque<"Option<&[u8]>">>
// CHECK: %[[NONE:.*]] = emitrust.method_call %[[CP]]["is_none"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<&[u8]>">>) -> i1
// CHECK: %[[CREF:.*]] = emitrust.method_call %[[CP]]["unwrap"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<&[u8]>">>) -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: emitrust.call_opaque "__emitrust_memcpy_u8"(

// TRUTH-TEST spelling in a `&&` conjunct (hmac_prng update's
// `if (data && datalen)` shape): the bare pointer truth test is the same
// is_some method call.
int feed(const uint8_t *data, unsigned int datalen) {
  uint8_t acc[8] = {0};
  if (data && datalen) {
    memcpy(acc, data, sizeof acc);
  }
  return (int)acc[0];
}

// CHECK-LABEL: func.func @feed
// CHECK-SAME: (%arg0: !emitrust.opaque<"Option<&[u8]>">, %arg1: ui32) -> i32
// CHECK: emitrust.method_call %{{.*}}["is_some"] ()
// CHECK: emitrust.method_call %{{.*}}["unwrap"] ()

// DECLINE side: the `(uint8_t *) 0` cast-null early exit whose use is a
// SUBSCRIPT (not a provably guarded byte-family source) keeps the plain
// slice classification — and now IMPORTS, because the cast spelling is
// recognized as a null pointer constant and the comparison FOLDS on the
// statically-non-null slice param (sound: a null-constant argument to
// `early` keeps its call-site rejection).
int early(const uint8_t *p, unsigned int len) {
  if (p == (uint8_t *)0 || len > 32u) {
    return -1;
  }
  return (int)p[0];
}

// CHECK-LABEL: func.func @early
// CHECK-SAME: (%arg0: !emitrust.ref<!emitrust.slice<ui8>>, %arg1: ui32) -> i32
// CHECK-NOT: emitrust.method_call
// CHECK: arith.constant false
// CHECK: return

// Call sites: an array region passes `Some(<region view>)`; C's null
// pointer constant passes an inline `None` — both polarities exercised,
// and each callee is called with TWO distinct arrays so the FR-40 owner
// lift stays out of the way.
int main(void) {
  uint8_t seed[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t alt[8] = {9, 10, 11, 12, 13, 14, 15, 16};
  uint8_t out[4];
  int r = init(out, seed, 8u);
  r += init(out, alt, 8u);
  r += init(out, 0, 0u);
  r += chk(seed, 8u);
  r += chk(alt, 8u);
  r += chk(0, 8u);
  r += feed(seed, 8u);
  r += feed(alt, 8u);
  r += feed(0, 0u);
  r += early(seed, 8u);
  r += early(alt, 8u);
  return r;
}

// CHECK-LABEL: func.func @c_main
// CHECK: %[[SEEDREF:.*]] = emitrust.slice_of %{{.*}} : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: %[[SOMEARG:.*]] = emitrust.call_opaque "Some"(%[[SEEDREF]]) : (!emitrust.ref<!emitrust.slice<ui8>>) -> !emitrust.opaque<"Option<&[u8]>">
// CHECK: call @init(%{{.*}}, %[[SOMEARG]], %{{.*}})
// CHECK: %[[NONEARG:.*]] = emitrust.constant <#emitrust.opaque<"None">> : !emitrust.opaque<"Option<&[u8]>">
// CHECK: call @init(%{{.*}}, %[[NONEARG]], %{{.*}})
// CHECK: call @chk(
// CHECK: call @feed(
// The declined param stays a plain shared slice at its call sites.
// CHECK: emitrust.slice_of %{{.*}} -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: call @early(

// The helper is emitted once, as safe Rust.
// CHECK: emitrust.verbatim "fn __emitrust_memcpy_u8(dst: &mut [u8], src: &[u8], n: i64)
// CHECK-NOT: unsafe

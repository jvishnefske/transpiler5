// RUN: emitrust-import-c %s | FileCheck %s

// FR-99: a FAM-record allocator whose `return NULL` is REACHABLE — not the
// elided malloc-failure guard, but a parameter validation (heatshrink's
// `heatshrink_encoder_alloc`, `heatshrink_decoder_alloc`) or the
// free-then-NULL arm of a member allocation — lifts to an OPTION-typed owned
// return instead of dropping to the historical returned-pointer rejection.
// FR-94 gave the infallible `-> !emitrust.struct<"S">` shape; this file pins
// the nullable one and the exactly two call-site shapes it admits:
//   1. The signature: `-> !emitrust.opaque<"Option<S>">`, built from the same
//      `Option<` + struct symbol spelling FR-96 uses for member position.
//   2. Every REACHABLE `return NULL` emits `emitrust.literal "None"`; every
//      owned return loads the claimed local and wraps it
//      `emitrust.call_opaque "Some"`. An elided-guard NULL still emits
//      NOTHING (the `vec!` malloc cannot fail).
//   3. The GUARDED BIND — `S *p = f(...);` immediately followed by
//      `if (p == NULL) ...` / `if (!p) ...` with no else — is NOT elided
//      (that would delete a reachable branch: a miscompile). The call result
//      lands in an Option temp, the guard folds to the FR-88/FR-96
//      `is_none`/`is_some` discriminant on that temp, and the payload
//      `unwrap` is DEFERRED into the guard's continuation, so the `None` arm
//      never touches the payload and the temp is moved exactly once.
//   4. The UNGUARDED BIND unwraps immediately at the binding — `unwrap()`'s
//      panic on None is the deterministic fail-loud refinement of C's
//      null-dereference undefined behaviour (the FR-88 argv/`as_mut().unwrap`
//      precedent).
//   5. Downstream of the unwrap NOTHING changes: the local is
//      `!emitrust.struct<"S">`, so member reads, tail subscripts, `&mut`
//      borrows and the owned move into a free wrapper all ride the
//      unmodified FR-94 machinery.
//   6. Composition with FR-96: a nullable allocator that also fills an
//      `Option<hs_index>` MEMBER keeps that member's failure guard faithful
//      (`is_none`, never elided) and returns `None` out of it — the exact
//      heatshrink_encoder_alloc shape, whose `free(e)` before the NULL is a
//      no-op drop.
//   7. The FR-94 INFALLIBLE callee is untouched: its caller-side guard is
//      still ELIDED (`CHECK-NOT: cf.cond_br`), because there the `None` is
//      unreachable. That regression pin is what makes the new conditional
//      elision honest.
// The frontier — every other call-site shape, returned cursors, returned
// parameters, mixed owned/borrowed returns, and a nullable result stored into
// an FR-96 member — stays in flexible-array-nullable-return-invalid.c.

#include <stdlib.h>

typedef struct {
  unsigned short n;
  unsigned char buf[];
} bag;

// CHECK: emitrust.struct_def @bag ["n", "buf"] [ui16, !emitrust.opaque<"Vec<u8>">]

// Pin 1/2: the validation NULL is reachable, so the signature is Option and
// that return site emits the None literal; the malloc guard's NULL still
// emits nothing at all, and `return b` wraps Some.
bag *bag_alloc(unsigned short n) {
  if (n == 0) {
    return NULL;
  }
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) {
    return NULL;
  }
  b->n = n;
  return b;
}

// CHECK-LABEL: func.func @bag_alloc(
// CHECK-SAME: -> !emitrust.opaque<"Option<bag>">
// CHECK: %[[NONE:.*]] = emitrust.literal "None" : !emitrust.opaque<"Option<bag>">
// CHECK: return %[[NONE]] : !emitrust.opaque<"Option<bag>">
// The elided malloc guard emits no second branch: exactly ONE cond_br (the
// validation test) reaches the emitted body.
// CHECK: emitrust.vec_fill
// CHECK: %[[OWNED:.*]] = emitrust.load %{{.*}} : (!emitrust.lvalue<!emitrust.struct<"bag">>) -> !emitrust.struct<"bag">
// CHECK: %[[SOME:.*]] = emitrust.call_opaque "Some"(%[[OWNED]]) : (!emitrust.struct<"bag">) -> !emitrust.opaque<"Option<bag>">
// CHECK: return %[[SOME]] : !emitrust.opaque<"Option<bag>">

// Pin 3: the guarded bind, `== NULL` polarity. The guard is a REAL branch on
// the Option discriminant, and the payload unwrap sits after it.
unsigned guarded_eq(unsigned short n) {
  bag *b = bag_alloc(n);
  if (b == NULL) {
    return 0;
  }
  unsigned v = b->n;
  free(b);
  return v;
}

// CHECK-LABEL: func.func @guarded_eq
// CHECK: %[[CALL:.*]] = call @bag_alloc(%{{.*}}) : (ui16) -> !emitrust.opaque<"Option<bag>">
// CHECK: %[[TMP:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.opaque<"Option<bag>">>
// CHECK: emitrust.assign %[[TMP]] = %[[CALL]] : !emitrust.lvalue<!emitrust.opaque<"Option<bag>">>
// CHECK: %[[ISN:.*]] = emitrust.method_call %[[TMP]]["is_none"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<bag>">>) -> i1
// CHECK: cf.cond_br %[[ISN]]
// CHECK: %[[UW:.*]] = emitrust.method_call %[[TMP]]["unwrap"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<bag>">>) -> !emitrust.struct<"bag">
// CHECK: emitrust.assign %{{.*}} = %[[UW]] : !emitrust.lvalue<!emitrust.struct<"bag">>

// Pin 3, `!p` polarity: the truth test is `is_some`, inverted by the
// existing logical-not lowering.
unsigned guarded_not(unsigned short n) {
  bag *b = bag_alloc(n);
  if (!b) {
    return 0;
  }
  unsigned v = b->buf[0];
  free(b);
  return v;
}

// CHECK-LABEL: func.func @guarded_not
// CHECK: %[[ISS:.*]] = emitrust.method_call %{{.*}}["is_some"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<bag>">>) -> i1
// CHECK: %[[INV:.*]] = arith.xori %[[ISS]]
// CHECK: cf.cond_br %[[INV]]
// CHECK: emitrust.method_call %{{.*}}["unwrap"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<bag>">>) -> !emitrust.struct<"bag">

// Pin 4/5: the unguarded bind unwraps at the binding, and every downstream
// access is the plain FR-94 owned-struct shape.
unsigned unguarded(unsigned short n) {
  bag *b = bag_alloc(n);
  unsigned v = b->n + b->buf[0];
  free(b);
  return v;
}

// CHECK-LABEL: func.func @unguarded
// CHECK-NOT: is_none
// CHECK: %[[UCALL:.*]] = call @bag_alloc(%{{.*}}) : (ui16) -> !emitrust.opaque<"Option<bag>">
// CHECK: emitrust.assign %{{.*}} = %[[UCALL]] : !emitrust.lvalue<!emitrust.opaque<"Option<bag>">>
// CHECK: %[[UUW:.*]] = emitrust.method_call %{{.*}}["unwrap"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<bag>">>) -> !emitrust.struct<"bag">
// CHECK: emitrust.assign %[[UPLACE:.*]] = %[[UUW]] : !emitrust.lvalue<!emitrust.struct<"bag">>
// CHECK: emitrust.member %[[UPLACE]]["n"]
// CHECK: emitrust.member %[[UPLACE]]["buf"]

// Pin 6: the heatshrink_encoder_alloc composition — an FR-96 Option member
// whose failure guard is FAITHFUL, and whose `free(e); return NULL;` arm
// becomes a bare `None` (the free is the no-op drop of an owned local).
struct hs_index {
  unsigned short size;
  short index[];
};

typedef struct {
  unsigned short n;
  struct hs_index *si;
  unsigned char buf[];
} enc;

// CHECK: emitrust.struct_def @enc ["n", "si", "buf"] [ui16, !emitrust.opaque<"Option<hs_index>">, !emitrust.opaque<"Vec<u8>">]

enc *enc_alloc(unsigned short n) {
  if (n == 0 || n > 64) {
    return NULL;
  }
  enc *e = malloc(sizeof(enc) + n);
  if (e == NULL) {
    return NULL;
  }
  e->n = n;
  e->si = malloc(sizeof(struct hs_index) + n * sizeof(short));
  if (e->si == NULL) {
    free(e);
    return NULL;
  }
  e->si->size = n;
  return e;
}

// CHECK-LABEL: func.func @enc_alloc(
// CHECK-SAME: -> !emitrust.opaque<"Option<enc>">
// CHECK: emitrust.literal "None" : !emitrust.opaque<"Option<enc>">
// The member allocation's own failure guard stays faithful (FR-96), and its
// arm returns None rather than being elided.
// CHECK: emitrust.call_opaque "Some"(%{{.*}}) : (!emitrust.struct<"hs_index">) -> !emitrust.opaque<"Option<hs_index>">
// CHECK: emitrust.method_call %{{.*}}["is_none"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<hs_index>">>) -> i1
// CHECK: emitrust.literal "None" : !emitrust.opaque<"Option<enc>">
// CHECK: emitrust.call_opaque "Some"(%{{.*}}) : (!emitrust.struct<"enc">) -> !emitrust.opaque<"Option<enc>">

unsigned enc_tally(unsigned short n) {
  enc *e = enc_alloc(n);
  if (e == NULL) {
    return 0;
  }
  unsigned s = e->si->size + e->buf[0];
  free(e->si);
  free(e);
  return s;
}

// CHECK-LABEL: func.func @enc_tally
// CHECK: call @enc_alloc(%{{.*}}) : (ui16) -> !emitrust.opaque<"Option<enc>">
// CHECK: emitrust.method_call %{{.*}}["is_none"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<enc>">>) -> i1
// CHECK: emitrust.method_call %{{.*}}["unwrap"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<enc>">>) -> !emitrust.struct<"enc">

// Pin 5, continued: a nullable-bound local is an ordinary owned local once
// unwrapped, so it CHAINS (a wrapper that guards an inner nullable allocator
// and re-returns the payload is itself nullable) and it MOVES into an owned
// free wrapper untouched by FR-99.
void bag_free(bag *b) { free(b); }

bag *bag_bump(unsigned short n) {
  bag *p = bag_alloc(n);
  if (p == NULL) {
    return NULL;
  }
  p->n = (unsigned short)(p->n + 1);
  return p;
}

// CHECK-LABEL: func.func @bag_bump(
// CHECK-SAME: -> !emitrust.opaque<"Option<bag>">
// CHECK: call @bag_alloc(%{{.*}}) : (ui16) -> !emitrust.opaque<"Option<bag>">
// CHECK: emitrust.method_call %{{.*}}["is_none"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<bag>">>) -> i1
// CHECK: emitrust.literal "None" : !emitrust.opaque<"Option<bag>">
// CHECK: emitrust.method_call %{{.*}}["unwrap"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<bag>">>) -> !emitrust.struct<"bag">
// CHECK: emitrust.call_opaque "Some"(%{{.*}}) : (!emitrust.struct<"bag">) -> !emitrust.opaque<"Option<bag>">

unsigned chained(unsigned short n) {
  bag *q = bag_bump(n);
  if (q == NULL) {
    return 0;
  }
  unsigned v = q->n;
  bag_free(q);
  return v;
}

// CHECK-LABEL: func.func @chained
// CHECK: call @bag_bump(%{{.*}}) : (ui16) -> !emitrust.opaque<"Option<bag>">
// CHECK: emitrust.method_call %{{.*}}["is_none"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<bag>">>) -> i1
// CHECK: %[[QUW:.*]] = emitrust.method_call %{{.*}}["unwrap"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<bag>">>) -> !emitrust.struct<"bag">
// CHECK: emitrust.assign %[[QP:.*]] = %[[QUW]] : !emitrust.lvalue<!emitrust.struct<"bag">>
// CHECK: %[[QMV:.*]] = emitrust.load %[[QP]] : (!emitrust.lvalue<!emitrust.struct<"bag">>) -> !emitrust.struct<"bag">
// CHECK: call @bag_free(%[[QMV]]) : (!emitrust.struct<"bag">) -> ()

// Pin 7 (REGRESSION): an INFALLIBLE FR-94 allocator keeps the bare struct
// return, and its caller's null guard is still ELIDED — the `None` there is
// unreachable, so folding it to a live branch would be a behaviour change in
// the other direction.
bag *bag_alloc_infallible(unsigned short n) {
  bag *b = malloc(sizeof(bag) + n);
  if (b == NULL) {
    return NULL;
  }
  b->n = n;
  return b;
}

// CHECK-LABEL: func.func @bag_alloc_infallible(
// CHECK-SAME: -> !emitrust.struct<"bag">

unsigned infallible_caller(unsigned short n) {
  bag *b = bag_alloc_infallible(n);
  if (b == NULL) {
    return 0;
  }
  unsigned v = b->n;
  free(b);
  return v;
}

// CHECK-LABEL: func.func @infallible_caller
// CHECK-NOT: cf.cond_br
// CHECK-NOT: is_none
// CHECK: call @bag_alloc_infallible(%{{.*}}) : (ui16) -> !emitrust.struct<"bag">

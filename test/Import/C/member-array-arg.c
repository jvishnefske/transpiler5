// RUN: emitrust-import-c %s | FileCheck %s

// FR-74: a member-array call argument (`bump(s.iv, s.n)` — tinycrypt's
// compress shape) decays to the SAME slice-parameter convention a
// top-level array argument uses: the argument lowers to an
// `emitrust.slice_of` over the member PLACE (`emitrust.member`) at
// cursor 0, and the callee's pointer parameter classifies as a slice
// reference through the ordinary Phase-1b fallback (member arrays can
// never owner-promote — promotion requires a plain local-array storage
// base — so the slice path is the only lowering and this pin keeps it
// honest). Pinned shapes: a byte (ui8) and a NON-byte (i32) element
// member array, the shared/const form, a NESTED dot chain, an ARROW
// base through a struct-pointer parameter, and the
// disjoint-sibling-fields shape — two member arrays of ONE struct into
// two mutable slice parameters in one call, which is provably
// non-overlapping in C and renders as a legal two-simultaneous-&mut
// borrow in Rust. Note main deliberately touches no member array
// element before the pinned calls, so each pinned `member` op is the
// argument's own projection.

struct S {
  unsigned char a[8];
  unsigned char b[8];
  int k[8];
  unsigned int n;
};

/* Mutable byte-slice callee: Phase-1b slice classification. */
// CHECK-LABEL: func.func @bump(
// CHECK-SAME: %{{[^:]+}}: !emitrust.mut_ref<!emitrust.slice<ui8>>
static void bump(unsigned char *buf, unsigned len) {
  unsigned i;
  for (i = 0; i < len; i++)
    buf[i] = (unsigned char)(buf[i] + 1u);
}

/* TWO mutable byte-slice parameters, fed by SIBLING fields below. */
// CHECK-LABEL: func.func @mix(
// CHECK-SAME: !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK-SAME: !emitrust.mut_ref<!emitrust.slice<ui8>>
static void mix(unsigned char *x, unsigned char *y, unsigned len) {
  unsigned i;
  for (i = 0; i < len; i++) {
    x[i] = (unsigned char)(x[i] + y[i]);
    y[i] = (unsigned char)(y[i] ^ x[i]);
  }
}

/* Shared (const) byte-slice callee. */
// CHECK-LABEL: func.func @csum(
// CHECK-SAME: %{{[^:]+}}: !emitrust.ref<!emitrust.slice<ui8>>
static unsigned csum(const unsigned char *p, unsigned len) {
  unsigned i, s = 0;
  for (i = 0; i < len; i++)
    s += p[i];
  return s;
}

/* NON-byte element type: the member array's i32 element must agree. */
// CHECK-LABEL: func.func @isum(
// CHECK-SAME: %{{[^:]+}}: !emitrust.mut_ref<!emitrust.slice<i32>>
static int isum(int *v, unsigned len) {
  v[0] = v[(int)len - 1] + 3;
  return v[0];
}

/* ARROW base: the struct-pointer parameter's member array decays the
   same way — slice_of over the deref'd member place at cursor 0. */
// CHECK-LABEL: func.func @compress(
// CHECK-SAME: %{{[^:]+}}: !emitrust.mut_ref<!emitrust.struct<"S">>
// CHECK: %[[PA:.+]] = emitrust.member %{{.+}}["a"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.array<8xui8>>
// CHECK-NEXT: %[[PC:.+]] = arith.constant 0 : i64
// CHECK-NEXT: %[[PS:.+]] = emitrust.slice_of mut %[[PA]][%[[PC]]] : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK-NEXT: call @bump(%[[PS]],
static void compress(struct S *s) {
  bump(s->a, s->n);
  /* Disjoint SIBLING fields of one struct in ONE call: admitted. */
  // CHECK: %[[XA:.+]] = emitrust.member %{{.+}}["a"]
  // CHECK: %[[XS:.+]] = emitrust.slice_of mut %[[XA]][%{{.+}}]
  // CHECK: %[[XB:.+]] = emitrust.member %{{.+}}["b"]
  // CHECK: %[[YS:.+]] = emitrust.slice_of mut %[[XB]][%{{.+}}]
  // CHECK-NEXT: call @mix(%[[XS]], %[[YS]],
  mix(s->a, s->b, s->n);
}

/* NESTED dot chain: o.in.iv projects two members deep (the C field
   `in` renders as the raw-identifier-avoiding `in_`). */
struct Inner {
  unsigned char iv[8];
  unsigned int t;
};
struct Outer {
  struct Inner in;
  unsigned int n;
};

// CHECK-LABEL: func.func @c_main
int main(void) {
  struct S s;
  struct Outer o;
  s.n = 8u;
  o.n = 8u;
  o.in.t = 1u;
  /* Dot base over the local struct at cursor 0. */
  // CHECK: %[[MA:.+]] = emitrust.member %{{.+}}["a"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.array<8xui8>>
  // CHECK-NEXT: %[[C0:.+]] = arith.constant 0 : i64
  // CHECK-NEXT: %[[SA:.+]] = emitrust.slice_of mut %[[MA]][%[[C0]]] : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
  // CHECK-NEXT: call @bump(%[[SA]],
  bump(s.a, s.n);
  /* Shared borrow of the member place for the const parameter. */
  // CHECK: %[[MC:.+]] = emitrust.member %{{.+}}["a"]
  // CHECK-NEXT: %[[C1:.+]] = arith.constant 0 : i64
  // CHECK-NEXT: %[[SC:.+]] = emitrust.slice_of %[[MC]][%[[C1]]] : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
  // CHECK-NEXT: call @csum(%[[SC]],
  unsigned c = csum(s.a, s.n);
  /* Non-byte element member array. */
  // CHECK: %[[MK:.+]] = emitrust.member %{{.+}}["k"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.array<8xi32>>
  // CHECK-NEXT: %[[C2:.+]] = arith.constant 0 : i64
  // CHECK-NEXT: %[[SK:.+]] = emitrust.slice_of mut %[[MK]][%[[C2]]] : (!emitrust.lvalue<!emitrust.array<8xi32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  // CHECK-NEXT: call @isum(%[[SK]],
  int r = isum(s.k, 8u);
  /* Nested dot chain: two member projections, then the slice. */
  // CHECK: %[[MI:.+]] = emitrust.member %{{.+}}["in_"] : (!emitrust.lvalue<!emitrust.struct<"Outer">>) -> !emitrust.lvalue<!emitrust.struct<"Inner">>
  // CHECK-NEXT: %[[MIV:.+]] = emitrust.member %[[MI]]["iv"] : (!emitrust.lvalue<!emitrust.struct<"Inner">>) -> !emitrust.lvalue<!emitrust.array<8xui8>>
  // CHECK-NEXT: %[[C3:.+]] = arith.constant 0 : i64
  // CHECK-NEXT: %[[SIV:.+]] = emitrust.slice_of mut %[[MIV]][%[[C3]]]
  // CHECK-NEXT: call @bump(%[[SIV]],
  bump(o.in.iv, o.n);
  // CHECK: call @compress(
  compress(&s);
  return (int)(c & 3u) + (r & 3);
}

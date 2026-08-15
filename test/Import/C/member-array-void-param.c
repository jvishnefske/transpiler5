// RUN: emitrust-import-c %s | FileCheck %s

// FR-89: a MEMBER-ARRAY call argument to a void*-ADMITTED byte-cursor
// parameter (FR-71's body-scan admission) lowers exactly like the same
// argument to a spelled-out byte-pointer parameter (FR-74/86):
// `emitrust.slice_of [mut]` over the member PLACE at the argument's
// cursor. The AST differs from the typed case by ONE node — the
// implicit void* BitCast Sema inserts at the conversion site
// (`bitcast<void*>(decay(member))`, tinycrypt's
// `_set(prng->key, 0x00, sizeof(prng->key))` shape at hmac_prng.c:143
// and the `tc_hmac_update(&prng->h, prng->v, ...)` shape at :88) — and
// the FR-71 call-site coercion predated the member-place matcher, so
// every one of these spellings kept the historical decay rejection.
// This pins the composed path: the void*-mediated cast peels on the
// borrow-ARGUMENT path only, then the FR-74/86 matcher, the element
// agreement against the admitted byte element, and the (root,
// field-path) aliasing guard all compose unchanged. Pinned shapes: dot
// root at cursor 0, dot-root constant offset (`s.iv + 4`), dot-root
// addr-of-subscript with a RUNTIME PURE index (shared borrow for the
// const param), arrow root through a struct-pointer parameter at cursor
// 0 and with a constant offset, disjoint SIBLING fields of one MIXED
// struct into two mutable void* params in one call, and — proving the
// peel re-routes NOTHING outside the member seam — a string literal to
// an i8-element void* param keeps its exact pre-FR-89 lowering (shared
// slice of a CONST literal backing via the pointer-rvalue path, not the
// literal head's fresh mutable backing). The structs are MIXED
// (byte arrays plus a non-byte field) on purpose: an ALL-u8 struct is
// the byte-region-aggregate boundary and stays rejected (see
// member-array-void-param-invalid.c). Note main touches no member
// array element before the pinned calls, so each pinned `member` op is
// the argument's own projection.

struct S {
  unsigned char iv[16];
  unsigned char tag[8];
  unsigned int n;
};

/* Mutable void* callee: FR-71 body-scan admission (u8 pointee). */
// CHECK-LABEL: func.func @bset(
// CHECK-SAME: %{{[^:]+}}: !emitrust.mut_ref<!emitrust.slice<ui8>>
static void bset(void *to, unsigned char val, unsigned len) {
  unsigned char *d = to;
  unsigned i;
  for (i = 0; i < len; i++)
    d[i] = val;
}

/* TWO mutable void* params, fed by SIBLING fields below. */
// CHECK-LABEL: func.func @two(
// CHECK-SAME: !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK-SAME: !emitrust.mut_ref<!emitrust.slice<ui8>>
static void two(void *a, void *b, unsigned len) {
  unsigned char *x = a;
  unsigned char *y = b;
  unsigned i;
  for (i = 0; i < len; i++) {
    x[i] = (unsigned char)(x[i] + y[i]);
    y[i] = (unsigned char)(y[i] ^ x[i]);
  }
}

/* Shared (const void*) callee. */
// CHECK-LABEL: func.func @csum(
// CHECK-SAME: %{{[^:]+}}: !emitrust.ref<!emitrust.slice<ui8>>
static unsigned csum(const void *from, unsigned len) {
  const unsigned char *p = from;
  unsigned i, s = 0;
  for (i = 0; i < len; i++)
    s += p[i];
  return s;
}

/* i8-element void* callee for the literal no-reroute pin. */
// CHECK-LABEL: func.func @firstc(
// CHECK-SAME: %{{[^:]+}}: !emitrust.ref<!emitrust.slice<i8>>
static int firstc(const void *p) {
  const char *c = (const char *)p;
  return c[0];
}

/* ARROW root: the struct-pointer parameter's member array reaches the
   void* param through the same peeled decay — slice_of over the deref'd
   member place, cursor 0 and the constant-offset cursor. */
// CHECK-LABEL: func.func @touch(
// CHECK-SAME: %{{[^:]+}}: !emitrust.mut_ref<!emitrust.struct<"S">>
// CHECK: %[[PA:.+]] = emitrust.member %{{.+}}["iv"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.array<16xui8>>
// CHECK-NEXT: %[[PC:.+]] = arith.constant 0 : i64
// CHECK-NEXT: %[[PS:.+]] = emitrust.slice_of mut %[[PA]][%[[PC]]] : (!emitrust.lvalue<!emitrust.array<16xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK-NEXT: call @bset(%[[PS]],
static void touch(struct S *p) {
  bset(p->iv, 9, 16u);
  /* ARROW-root constant offset: the `+ 4` folds to a literal cursor. */
  // CHECK: %[[OA:.+]] = emitrust.member %{{.+}}["iv"]
  // CHECK-NEXT: %[[OC:.+]] = arith.constant 4 : i64
  // CHECK-NEXT: %[[OS:.+]] = emitrust.slice_of mut %[[OA]][%[[OC]]]
  // CHECK-NEXT: call @bset(%[[OS]],
  bset(p->iv + 4, 7, 4u);
  /* Disjoint SIBLING fields of one struct in ONE call: admitted, two
     simultaneous mutable borrows of provably non-overlapping fields. */
  // CHECK: %[[XA:.+]] = emitrust.member %{{.+}}["iv"]
  // CHECK: %[[XS:.+]] = emitrust.slice_of mut %[[XA]][%{{.+}}]
  // CHECK: %[[XB:.+]] = emitrust.member %{{.+}}["tag"]
  // CHECK: %[[YS:.+]] = emitrust.slice_of mut %[[XB]][%{{.+}}]
  // CHECK-NEXT: call @two(%[[XS]], %[[YS]],
  two(p->iv, p->tag, 8u);
}

// CHECK-LABEL: func.func @c_main
int main(void) {
  struct S s;
  s.n = 12u;
  /* DOT root at cursor 0, mutable. */
  // CHECK: %[[MA:.+]] = emitrust.member %{{.+}}["iv"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.array<16xui8>>
  // CHECK-NEXT: %[[C0:.+]] = arith.constant 0 : i64
  // CHECK-NEXT: %[[SA:.+]] = emitrust.slice_of mut %[[MA]][%[[C0]]] : (!emitrust.lvalue<!emitrust.array<16xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
  // CHECK-NEXT: call @bset(%[[SA]],
  bset(s.iv, 1, 16u);
  /* DOT-root constant offset (`s.iv + 4`): literal i64 cursor 4. */
  // CHECK: %[[OA2:.+]] = emitrust.member %{{.+}}["iv"]
  // CHECK-NEXT: %[[OC2:.+]] = arith.constant 4 : i64
  // CHECK-NEXT: %[[OS2:.+]] = emitrust.slice_of mut %[[OA2]][%[[OC2]]]
  // CHECK-NEXT: call @bset(%[[OS2]],
  bset(s.iv + 4, 2, 4u);
  /* DOT-root addr-of-subscript, RUNTIME PURE index (`&s.iv[s.n - 4u]`,
     the member-load index): shared borrow for the const void* param. */
  // CHECK: %[[QA:.+]] = emitrust.member %{{.+}}["iv"]
  // CHECK: %[[QC:.+]] = emitrust.cast %{{.+}} : ui32 to i64
  // CHECK-NEXT: %[[QS:.+]] = emitrust.slice_of %[[QA]][%[[QC]]]
  // CHECK-NEXT: call @csum(%[[QS]],
  unsigned c = csum(&s.iv[s.n - 4u], 4u);
  /* String literal to the i8-element void* param: byte-identical to the
     pre-FR-89 lowering — CONST literal backing, shared slice, cursor 0
     (the peel sits BETWEEN the literal head and the member matcher, so
     it cannot re-route this shape to a fresh mutable backing). */
  // CHECK: %[[LB:.+]] = emitrust.variable const <[97 : i8, 98 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<3xi8>>
  // CHECK: %[[LS:.+]] = emitrust.slice_of %[[LB]][%{{.+}}] : (!emitrust.lvalue<!emitrust.array<3xi8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>
  // CHECK-NEXT: call @firstc(%[[LS]])
  int f = firstc("ab");
  // CHECK: call @touch(
  touch(&s);
  return (int)(c & 7u) + (f & 1);
}

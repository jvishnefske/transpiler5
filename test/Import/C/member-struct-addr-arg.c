// RUN: split-file %s %t
// RUN: emitrust-import-c %t/slice-rep.c | FileCheck %s --check-prefix=SLICE
// RUN: emitrust-import-c %t/owner-rep.c | FileCheck %s --check-prefix=OWNER
// RUN: emitrust-import-c --externals-trait %t/shared-req.c %t/empty.c \
// RUN:   | FileCheck %s --check-prefix=SHARED

// FR-90: a member-ADDRESS argument (`&s->field`, field STRUCT-typed) to
// a struct-pointer parameter, through a NULL-COMPARED — and therefore
// DECOMPOSED — struct-pointer root. This is the struct-typed sibling of
// FR-74/86's member-ARRAY arguments and the exact residual behind every
// remaining "unsupported pointer target expression" corpus site (7
// sites, all tinycrypt: ctr_prng's `tc_aes_encrypt(..., &ctx->key)` x3,
// hmac's `tc_sha256_init(&ctx->hash_state)` x3, hmac_prng's
// `tc_hmac_init(&prng->h)` at :210). The FR-90 spike REFUTED the "loop
// context breaks it" framing: `&s->field` through a NON-null-checked
// ref/mut_ref param already borrows via emitLValue, in and out of
// loops; the trigger is the null compare, which demotes the root to the
// (slice-of-struct base, i64 cursor) decomposition where
// classifyMemberAddress has no arrow-root representation. The admitted
// lowering builds the member place exactly the way FR-86 mechanism A
// already does for member arrays — subscript the region base at the
// CURRENT cursor, then project the field chain — and borrows THAT place
// at the parameter's own mutability. Pinned per decomposed
// representation (the Phase-1b SLICE form a multi-base caller produces
// — the corpus's --incremental shape — and the OWNER-method form): the
// straight-line mutable borrow, the SAME shape inside a while loop
// (hmac_prng 210 — a fresh borrow per iteration, nothing loop-specific),
// the disjoint-sibling double borrow (`two(&s->h, &s->g)` — two struct
// fields of ONE decomposed root in one call, rustc-verified legal in the
// FR-90 spike), the MIXED addr_of + slice_of pair over one root
// (`mixed(&s->h, s->key, ...)` — the real hmac_prng generate() shape,
// admitted by the (root, field-path) guard's disjoint-prefix compare),
// a NESTED dot link under the arrow root (`&s->d.l` — the field-path
// machinery admits it trivially), and the const-POINTEE spellings of
// BOTH borrow shapes: a DEFINED `const struct Inner *` parameter keeps
// the historical `&mut T` convention (FR-55 leaves non-u8 const
// pointees mutable; the argument's qualification NoOp is peeled inside
// the interception only, so the borrow is mut), while the FR-80
// requirement convention — a body-less const-struct-pointee declaration
// under --externals-trait — is the ONE C spelling that yields a true
// SHARED `!emitrust.ref<struct>` parameter, and the member address then
// borrows SHARED through the same decomposed place (its own SHARED
// prefix below). The folded-to-false null compare is pinned so the
// demotion context stays visible.

//--- slice-rep.c

/* Library TU (no main, external linkage): the null-compared parameter
   classifies as a slice-of-struct region — the corpus's shape. */

struct Inner {
  unsigned int a;
  unsigned int b;
};
struct Deep {
  struct Inner l;
  unsigned int t;
};
struct C {
  struct Inner h;
  struct Inner g;
  struct Deep d;
  unsigned char key[8];
  unsigned int n;
};

/* Mutable struct-pointer callee (no null check of its own, so it keeps
   the mut_ref receiver convention — tinycrypt's cross-TU decl shape). */
// SLICE-LABEL: func.func @bump(
// SLICE-SAME: !emitrust.mut_ref<!emitrust.struct<"Inner">>
void bump(struct Inner *x, unsigned int d) {
  x->a += d;
  x->b ^= x->a;
}

/* A DEFINED const-pointee callee: FR-55 keeps non-u8 const pointees on
   the historical mut_ref convention, so the borrow below is mut and the
   argument's qualification NoOp is peeled by the interception. */
// SLICE-LABEL: func.func @fold(
// SLICE-SAME: !emitrust.mut_ref<!emitrust.struct<"Inner">>
unsigned int fold(const struct Inner *x) {
  return x->a * 3u + x->b;
}

/* Two mutable struct-pointer parameters: the disjoint-sibling target. */
// SLICE-LABEL: func.func @two(
void two(struct Inner *x, struct Inner *y) {
  x->a += y->b;
  y->b ^= x->a;
}

/* Mutable struct borrow + shared byte slice: the mixed-pair target. */
// SLICE-LABEL: func.func @mixed(
void mixed(struct Inner *x, const unsigned char *k, unsigned int len) {
  unsigned int i;
  for (i = 0u; i < len; i++)
    x->b += (unsigned int)k[i] * (i + 1u);
}

// SLICE-LABEL: func.func @update(
// SLICE-SAME: %{{[^:]+}}: !emitrust.mut_ref<!emitrust.slice<!emitrust.struct<"C">>>
void update(struct C *s) {
  /* The null compare folds to a constant false discriminant. */
  // SLICE: arith.constant false
  if (s == (struct C *)0)
    return;
  /* Straight-line mutable member-address borrow through the decomposed
     root: subscript-at-cursor -> member -> addr_of mut. */
  // SLICE: %[[P1:.+]] = emitrust.subscript %{{.+}}[%{{.+}}] : (!emitrust.lvalue<!emitrust.slice<!emitrust.struct<"C">>>, i64) -> !emitrust.lvalue<!emitrust.struct<"C">>
  // SLICE-NEXT: %[[H1:.+]] = emitrust.member %[[P1]]["h"] : (!emitrust.lvalue<!emitrust.struct<"C">>) -> !emitrust.lvalue<!emitrust.struct<"Inner">>
  // SLICE-NEXT: %[[R1:.+]] = emitrust.addr_of mut %[[H1]] : (!emitrust.lvalue<!emitrust.struct<"Inner">>) -> !emitrust.mut_ref<!emitrust.struct<"Inner">>
  // SLICE-NEXT: call @bump(%[[R1]],
  bump(&s->h, 3u);
  /* The SAME shape inside a while loop (hmac_prng.c:210): a fresh
     borrow per iteration, nothing loop-specific — the loop is still CFG
     form (`cf.cond_br`) at import time. */
  // SLICE: cf.cond_br
  // SLICE: %[[H2:.+]] = emitrust.member %{{.+}}["h"]
  // SLICE-NEXT: %[[R2:.+]] = emitrust.addr_of mut %[[H2]]
  // SLICE-NEXT: call @bump(%[[R2]],
  while (s->n < 4u) {
    bump(&s->h, s->n);
    s->n = s->n + 1u;
  }
  /* Disjoint sibling fields of ONE decomposed root in one call. */
  // SLICE: %[[H3:.+]] = emitrust.member %{{.+}}["h"]
  // SLICE-NEXT: %[[R3:.+]] = emitrust.addr_of mut %[[H3]]
  // SLICE: %[[G4:.+]] = emitrust.member %{{.+}}["g"]
  // SLICE-NEXT: %[[R4:.+]] = emitrust.addr_of mut %[[G4]]
  // SLICE-NEXT: call @two(%[[R3]], %[[R4]])
  two(&s->h, &s->g);
  /* MIXED pair over one root: addr_of of one field, slice_of of a
     sibling member array — the hmac_prng generate() shape. */
  // SLICE: %[[H5:.+]] = emitrust.member %{{.+}}["h"]
  // SLICE-NEXT: %[[R5:.+]] = emitrust.addr_of mut %[[H5]]
  // SLICE: %[[K6:.+]] = emitrust.member %{{.+}}["key"]
  // SLICE-NEXT: %[[Z6:.+]] = arith.constant 0 : i64
  // SLICE-NEXT: %[[S6:.+]] = emitrust.slice_of %[[K6]][%[[Z6]]] : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
  // SLICE-NEXT: call @mixed(%[[R5]], %[[S6]],
  mixed(&s->h, s->key, 8u);
  /* NESTED dot link under the arrow root: path [d, l]. */
  // SLICE: %[[D7:.+]] = emitrust.member %{{.+}}["d"]
  // SLICE-NEXT: %[[L7:.+]] = emitrust.member %[[D7]]["l"] : (!emitrust.lvalue<!emitrust.struct<"Deep">>) -> !emitrust.lvalue<!emitrust.struct<"Inner">>
  // SLICE-NEXT: %[[R7:.+]] = emitrust.addr_of mut %[[L7]]
  // SLICE-NEXT: call @bump(%[[R7]],
  bump(&s->d.l, 1u);
  /* Const-POINTEE defined callee: the qualification NoOp is peeled and
     the borrow stays MUT (the parameter's own convention). */
  // SLICE: %[[H8:.+]] = emitrust.member %{{.+}}["h"]
  // SLICE-NEXT: %[[R8:.+]] = emitrust.addr_of mut %[[H8]] : (!emitrust.lvalue<!emitrust.struct<"Inner">>) -> !emitrust.mut_ref<!emitrust.struct<"Inner">>
  // SLICE-NEXT: %{{.+}} = call @fold(%[[R8]])
  s->n = fold(&s->h) & 7u;
}

//--- owner-rep.c

/* A single local struct-array driver OWNER-promotes: `update` becomes a
   method whose region base is the receiver's data member — the other
   spelling of the same decomposed (base place + cursor) model. */

struct Inner {
  unsigned int a;
  unsigned int b;
};
struct C {
  struct Inner h;
  struct Inner g;
  unsigned int n;
};

// OWNER-LABEL: func.func @bump(
// OWNER-SAME: !emitrust.mut_ref<!emitrust.struct<"Inner">>
static void bump(struct Inner *x, unsigned int d) {
  x->a += d;
  x->b ^= x->a;
}

// OWNER-LABEL: func.func @fold(
// OWNER-SAME: !emitrust.mut_ref<!emitrust.struct<"Inner">>
static unsigned int fold(const struct Inner *x) {
  return x->a * 3u + x->b;
}

// OWNER-LABEL: func.func @update(
// OWNER-SAME: %{{[^:]+}}: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>
// OWNER-SAME: emitrust.method_of = "Owner_main_arr"
static void update(struct C *s) {
  // OWNER: %[[DATA:.+]] = emitrust.member %{{.+}}["data"] : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.array<1x!emitrust.struct<"C">>>
  // OWNER: arith.constant false
  if (s == (struct C *)0)
    return;
  /* Same borrow lowering over the owner's data array place. */
  // OWNER: %[[P1:.+]] = emitrust.subscript %[[DATA]][%{{.+}}] : (!emitrust.lvalue<!emitrust.array<1x!emitrust.struct<"C">>>, i64) -> !emitrust.lvalue<!emitrust.struct<"C">>
  // OWNER-NEXT: %[[H1:.+]] = emitrust.member %[[P1]]["h"]
  // OWNER-NEXT: %[[R1:.+]] = emitrust.addr_of mut %[[H1]]
  // OWNER-NEXT: call @bump(%[[R1]],
  bump(&s->h, 3u);
  /* Disjoint sibling in the same statement list (fresh borrow; the
     value argument reads s->h.a through its own member chain first). */
  // OWNER: %[[G2:.+]] = emitrust.member %{{.+}}["g"]
  // OWNER-NEXT: %[[R2:.+]] = emitrust.addr_of mut %[[G2]]
  // OWNER-NEXT: call @bump(%[[R2]],
  bump(&s->g, s->h.a & 7u);
  /* Const-POINTEE defined callee through the owner data place: the
     qualification NoOp is peeled, the borrow stays MUT. */
  // OWNER: %[[H3:.+]] = emitrust.member %{{.+}}["h"]
  // OWNER-NEXT: %[[R3:.+]] = emitrust.addr_of mut %[[H3]]
  // OWNER-NEXT: %{{.+}} = call @fold(%[[R3]])
  s->n = s->n + (fold(&s->h) & 7u);
}

// OWNER-LABEL: func.func @c_main
int main(void) {
  struct C arr[1];
  arr[0].h.a = 1u;
  arr[0].h.b = 2u;
  arr[0].g.a = 3u;
  arr[0].g.b = 4u;
  arr[0].n = 0u;
  // OWNER: call @update(
  update(arr);
  update(arr);
  return (int)(arr[0].n & 1u);
}

//--- shared-req.c

/* FR-80 requirement convention: a body-less const-struct-pointee
   declaration under --externals-trait maps to a true SHARED
   `!emitrust.ref<struct>` requirement parameter — the one C spelling of
   a shared struct borrow — and the member address of the decomposed
   root borrows SHARED through the same subscript-at-cursor place. */

struct Inner {
  unsigned int a;
  unsigned int b;
};
struct Outer {
  struct Inner h;
  unsigned int n;
};

// SHARED: func.func private @fold(!emitrust.ref<!emitrust.struct<"Inner">>) -> ui32 attributes {emitrust.external_requirement}
unsigned int fold(const struct Inner *x);

// SHARED-LABEL: func.func @drive(
// SHARED-SAME: !emitrust.mut_ref<!emitrust.slice<!emitrust.struct<"Outer">>>
unsigned int drive(struct Outer *o) {
  // SHARED: arith.constant false
  if (o == (struct Outer *)0)
    return 1u;
  // SHARED: %[[P:.+]] = emitrust.subscript %{{.+}}[%{{.+}}] : (!emitrust.lvalue<!emitrust.slice<!emitrust.struct<"Outer">>>, i64) -> !emitrust.lvalue<!emitrust.struct<"Outer">>
  // SHARED-NEXT: %[[H:.+]] = emitrust.member %[[P]]["h"]
  // SHARED-NEXT: %[[R:.+]] = emitrust.addr_of %[[H]] : (!emitrust.lvalue<!emitrust.struct<"Inner">>) -> !emitrust.ref<!emitrust.struct<"Inner">>
  // SHARED-NEXT: %{{.+}} = call @fold(%[[R]])
  return fold(&o->h);
}

//--- empty.c
/* Second TU: --externals-trait requirement promotion is a project-level
   decision (the merged module must not define main). */

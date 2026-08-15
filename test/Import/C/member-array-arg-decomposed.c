// RUN: split-file %s %t
// RUN: emitrust-import-c %t/slice-rep.c | FileCheck %s --check-prefix=SLICE
// RUN: emitrust-import-c %t/owner-rep.c | FileCheck %s --check-prefix=OWNER

// FR-86 (mechanism A): a NULL-COMPARED struct-pointer parameter
// (`if (s == (struct C *)0) return;` — tinycrypt null-checks every
// context pointer) demotes from the plain ref/mut_ref-struct
// representation to a DECOMPOSED one, and FR-74's member-array
// interception previously bailed on every decomposed root. This was
// the DOMINANT residual mechanism behind the corpus's remaining
// ArrayToPointerDecay stubs (5 of 8 sites: sha256's
// `compress(s->iv, s->leftover)`, ctr_prng's `arrInc(ctx->V, ...)`,
// both aes round-key calls, cmac's offset copy). The admitted lowering
// builds the member place exactly the way `s->n` already lowers —
// subscript the region base at the CURRENT cursor, then project the
// member — and passes `slice_of` over that place. BOTH decomposed
// representations are pinned, because the caller shape picks one: the
// Phase-1b SLICE decomposition (`!emitrust.mut_ref<!emitrust.slice<
// struct>>` + i64 cursor cell — what a library TU or a multi-base
// caller produces, the corpus's --incremental shape) and the
// OWNER-method form (the promoted local struct array's receiver data
// place + cursor parameter). Pinned per representation: the
// dual-argument disjoint-sibling-fields call (`compress_(s->iv,
// s->leftover)` — two fields of ONE decomposed root in one call,
// provably non-overlapping in C, rustc-verified as a legal
// two-simultaneous-borrow in the FR-86 spike), a mutable single-member
// borrow to a callee that itself null-checks its slice (`arrInc`), and
// the A+B combination — an FR-86 OFFSET into a member of the
// decomposed root with a runtime PURE member-load index (tinycrypt's
// cmac `&s->leftover[...]` shape). The folded-to-false null compare is
// pinned so the demotion context stays visible.

//--- slice-rep.c

/* Library TU (no main, external linkage): the null-compared parameter
   classifies as a slice-of-struct region — the corpus's shape. */

struct C {
  unsigned int iv[4];
  unsigned char leftover[8];
  unsigned int n;
};

/* Mutable u32-slice + shared byte-slice callee: the dual-borrow target. */
// SLICE-LABEL: func.func @compress_(
// SLICE-SAME: !emitrust.mut_ref<!emitrust.slice<ui32>>
// SLICE-SAME: !emitrust.ref<!emitrust.slice<ui8>>
void compress_(unsigned int *iv, const unsigned char *data) {
  unsigned int i;
  for (i = 0u; i < 4u; i++)
    iv[i] += (unsigned int)data[i] * (i + 1u);
}

/* The callee's OWN null check of its slice parameter folds away too
   (tinycrypt's arrInc guards `if (0 != arr)`). */
// SLICE-LABEL: func.func @arrInc(
// SLICE-SAME: !emitrust.mut_ref<!emitrust.slice<ui8>>
void arrInc(unsigned char *arr, unsigned int len) {
  unsigned int i;
  if (0 != arr) {
    for (i = len; i > 0u; i--) {
      arr[i - 1u] = (unsigned char)(arr[i - 1u] + 1u);
      if (arr[i - 1u] != 0u)
        break;
    }
  }
}

/* Shared byte-slice callee for the offset (A+B) shape. */
// SLICE-LABEL: func.func @csum_(
// SLICE-SAME: %{{[^:]+}}: !emitrust.ref<!emitrust.slice<ui8>>
unsigned int csum_(const unsigned char *p, unsigned int len) {
  unsigned int i, s = 0u;
  for (i = 0u; i < len; i++)
    s += p[i];
  return s;
}

// SLICE-LABEL: func.func @update(
// SLICE-SAME: %{{[^:]+}}: !emitrust.mut_ref<!emitrust.slice<!emitrust.struct<"C">>>
void update(struct C *s) {
  /* The null compare folds to a constant false discriminant. */
  // SLICE: arith.constant false
  if (s == (struct C *)0)
    return;
  /* Dual disjoint-sibling borrow through the decomposed root:
     subscript-at-cursor -> member -> slice_of, per argument. */
  // SLICE: %[[P1:.+]] = emitrust.subscript %{{.+}}[%{{.+}}] : (!emitrust.lvalue<!emitrust.slice<!emitrust.struct<"C">>>, i64) -> !emitrust.lvalue<!emitrust.struct<"C">>
  // SLICE-NEXT: %[[IV:.+]] = emitrust.member %[[P1]]["iv"] : (!emitrust.lvalue<!emitrust.struct<"C">>) -> !emitrust.lvalue<!emitrust.array<4xui32>>
  // SLICE-NEXT: %[[Z1:.+]] = arith.constant 0 : i64
  // SLICE-NEXT: %[[S1:.+]] = emitrust.slice_of mut %[[IV]][%[[Z1]]] : (!emitrust.lvalue<!emitrust.array<4xui32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui32>>
  // SLICE: %[[P2:.+]] = emitrust.subscript
  // SLICE-NEXT: %[[LO:.+]] = emitrust.member %[[P2]]["leftover"]
  // SLICE-NEXT: %[[Z2:.+]] = arith.constant 0 : i64
  // SLICE-NEXT: %[[S2:.+]] = emitrust.slice_of %[[LO]][%[[Z2]]] : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
  // SLICE-NEXT: call @compress_(%[[S1]], %[[S2]])
  compress_(s->iv, s->leftover);
  /* Single mutable member borrow through the decomposed root. */
  // SLICE: %[[P3:.+]] = emitrust.subscript
  // SLICE-NEXT: %[[LO2:.+]] = emitrust.member %[[P3]]["leftover"]
  // SLICE-NEXT: %[[Z3:.+]] = arith.constant 0 : i64
  // SLICE-NEXT: %[[S3:.+]] = emitrust.slice_of mut %[[LO2]][%[[Z3]]]
  // SLICE-NEXT: call @arrInc(%[[S3]],
  arrInc(s->leftover, 8u);
  /* A+B: OFFSET into a member of the decomposed root, runtime PURE
     member-load index (`&s->leftover[s->n & 3u]` — the cmac shape). */
  // SLICE: %[[LO3:.+]] = emitrust.member %{{.+}}["leftover"]
  // SLICE: %[[CI:.+]] = emitrust.cast %{{.+}} : ui32 to i64
  // SLICE-NEXT: %[[S4:.+]] = emitrust.slice_of %[[LO3]][%[[CI]]] : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
  // SLICE-NEXT: %{{.+}} = call @csum_(%[[S4]],
  s->n = csum_(&s->leftover[s->n & 3u], 2u) & 7u;
}

//--- owner-rep.c

/* A single local struct-array driver OWNER-promotes: `update` becomes
   a method whose region base is the receiver's data member — the other
   spelling of the same decomposed (base place + cursor) model. */

struct C {
  unsigned int iv[4];
  unsigned char leftover[8];
  unsigned int n;
};

// OWNER-LABEL: func.func @compress_(
// OWNER-SAME: !emitrust.mut_ref<!emitrust.slice<ui32>>
// OWNER-SAME: !emitrust.ref<!emitrust.slice<ui8>>
static void compress_(unsigned int *iv, const unsigned char *data) {
  unsigned int i;
  for (i = 0u; i < 4u; i++)
    iv[i] += (unsigned int)data[i] * (i + 1u);
}

// OWNER-LABEL: func.func @update(
// OWNER-SAME: %{{[^:]+}}: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>
// OWNER-SAME: emitrust.method_of = "Owner_main_arr"
static void update(struct C *s) {
  // OWNER: %[[DATA:.+]] = emitrust.member %{{.+}}["data"] : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.array<1x!emitrust.struct<"C">>>
  // OWNER: arith.constant false
  if (s == (struct C *)0)
    return;
  /* Same dual-borrow lowering over the owner's data array place. */
  // OWNER: %[[P1:.+]] = emitrust.subscript %[[DATA]][%{{.+}}] : (!emitrust.lvalue<!emitrust.array<1x!emitrust.struct<"C">>>, i64) -> !emitrust.lvalue<!emitrust.struct<"C">>
  // OWNER-NEXT: %[[IV:.+]] = emitrust.member %[[P1]]["iv"]
  // OWNER-NEXT: %[[Z1:.+]] = arith.constant 0 : i64
  // OWNER-NEXT: %[[S1:.+]] = emitrust.slice_of mut %[[IV]][%[[Z1]]]
  // OWNER: %[[P2:.+]] = emitrust.subscript %[[DATA]][%{{.+}}]
  // OWNER-NEXT: %[[LO:.+]] = emitrust.member %[[P2]]["leftover"]
  // OWNER-NEXT: %[[Z2:.+]] = arith.constant 0 : i64
  // OWNER-NEXT: %[[S2:.+]] = emitrust.slice_of %[[LO]][%[[Z2]]]
  // OWNER-NEXT: call @compress_(%[[S1]], %[[S2]])
  compress_(s->iv, s->leftover);
  s->n = s->n + 1u;
}

// OWNER-LABEL: func.func @c_main
int main(void) {
  struct C arr[1];
  unsigned int i;
  for (i = 0u; i < 4u; i++)
    arr[0].iv[i] = i * 3u;
  for (i = 0u; i < 8u; i++)
    arr[0].leftover[i] = (unsigned char)(250u + i);
  arr[0].n = 0u;
  // OWNER: call @update(
  update(arr);
  update(arr);
  return (int)(arr[0].n & 1u);
}

// RUN: emitrust-import-c %s | FileCheck %s

// Stage 5 of the owner-struct self-reference extension (design.md FR-30
// follow-on), import-only/FileCheck test — the master plan's B5
// sub-blocker: cross-parameter pointer equality, plus a plain-field
// mutation alongside the enum field, in that same cross-parameter-equality
// context. Builds directly on Stage 4
// (`test/Import/C/array-self-ref-member-chain.c`), which proved path
// compression (`find`) but never compared TWO INDEPENDENT `find` results
// against each other — the exact shape `union-find.c`'s `uf_union` needs
// and that Stage 4's own report flagged as still-blocking:
//   - Fix 1 (Pass A wiring gap): `planArrayMemberPointers`'s OWN
//     `PointerRegionAnalysis` instance (`lib/ImportC/ImportCPlanning.cpp`)
//     did not have `ownerIndexReturnQuery` wired to it, so a local bound
//     from an owner-index-returning call (`ra = find(a);`) could not be
//     resolved as rooting in the promoted array class during Pass A at
//     all, poisoning the field program-wide before emission was ever
//     reached. Fixed by wiring the SAME callback Pass A already wires for
//     `arrayMemberFieldQuery` (Stage 4).
//   - Fix 2 (B5 carve-out): even once Pass A could resolve `ra`/`rb`, the
//     general pointer-equality comparator (`lib/ImportC/ImportCExpressions.cpp`)
//     rejected `ra == rb` because each is registered with a DIFFERENT
//     `PtrExprValue::base` (the parameter each ultimately traces to — `a`
//     for `ra`, `b` for `rb` — since `find`'s return re-roots at whichever
//     pointer argument was passed). By `planOwners`'s all-or-nothing
//     per-function qualification, every data-pointer parameter of one
//     qualifying method (here, `unite`) is provably a cursor into the SAME
//     array class, so the two bases can never actually name different
//     objects within this one function body. Fixed with a narrow,
//     same-function-body-only carve-out: when both sides' bases are each
//     either the owner array itself or a data-pointer parameter of the
//     CURRENT method, the base-identity check is skipped and only the
//     cursor values are compared (equality/inequality only — never
//     generalized to ordered comparisons or across functions).

struct node { struct node *self; int rank; };

// Reused unchanged from Stage 4: binds one element's `self` field to
// another element (or itself).
void link_node(struct node *p, struct node *target) {
  p->self = target;
}

// Reused unchanged from Stage 4: path-compressing find.
struct node *find(struct node *x) {
  struct node *parent;
  while (x->self != x) {
    parent = x->self;
    x->self = parent->self;
    x = parent;
  }
  return x;
}

// The actual Stage 5 shape: `a` and `b` are two INDEPENDENT pointer
// parameters, each traced through its own `find` call to a local (`ra`,
// `rb`) that is compared for equality (B5) against the OTHER local. If
// unequal, one root's `self` field (the enum field) is repointed at the
// other AND the other's `rank` field (a plain sibling field) is
// incremented — "plain field mutation alongside enum field, in the
// cross-parameter-equality context" per the master plan's Stage 5
// description.
void unite(struct node *a, struct node *b) {
  struct node *ra = find(a);
  struct node *rb = find(b);
  if (ra == rb)
    return;
  ra->self = rb;
  rb->rank = rb->rank + 1;
}

int main(void) {
  struct node arr[4];
  // Two independent 2-element chains inside ONE promoted class: arr[0] is
  // its own root with arr[1] chained to it; arr[2] is its own root with
  // arr[3] chained to it.
  link_node(&arr[0], &arr[0]);
  link_node(&arr[1], &arr[0]);
  link_node(&arr[2], &arr[2]);
  link_node(&arr[3], &arr[2]);
  // Unites the two groups via two DIFFERENT parameters independently
  // traced back through `find`.
  unite(&arr[1], &arr[3]);
  return arr[2].rank;
}

// The synthesized enum, one variant per array element, shared by every
// site of the field.
// CHECK: emitrust.enum_def @node_self_Bases ["E0", "E1", "E2", "E3"] [0, 1, 2, 3] {unsigned_underlying}
// CHECK: emitrust.struct_def @node ["self_", "rank"] [!emitrust.enum<"node_self_Bases">, i32]

// `unite`: two independent calls into `find` bind `ra`/`rb`, whose
// cursors are then compared DIRECTLY — no base-identity rejection, no
// intermediate pointer-object machinery, exactly the general single-base
// cursor-comparison shape (Stage 5's B5 carve-out proves the two
// DIFFERENT parameter-rooted bases as belonging to one class instead of
// rejecting the comparison outright).
// CHECK-LABEL: func.func @unite
// CHECK-SAME: attributes {emitrust.method_of = "Owner_main_arr"}
// CHECK: %[[RA:.*]] = call @find
// CHECK: memref.store %[[RA]], %[[RACELL:.*]][]
// CHECK: %[[RB:.*]] = call @find
// CHECK: memref.store %[[RB]], %[[RBCELL:.*]][]
// CHECK: %[[RALOAD:.*]] = memref.load %[[RACELL]][]
// CHECK: %[[RBLOAD:.*]] = memref.load %[[RBCELL]][]
// CHECK: arith.cmpi eq, %[[RALOAD]], %[[RBLOAD]] : i64

// `ra->self = rb;`: the enum-field write, encoding `rb`'s own cursor.
// CHECK: emitrust.switch %{{.*}} : i64
// CHECK-NEXT: case 0 {
// CHECK: node_self_Bases::E0
// CHECK: case 1 {
// CHECK: node_self_Bases::E1
// CHECK: case 2 {
// CHECK: node_self_Bases::E2
// CHECK: case 3 {
// CHECK: node_self_Bases::E3
// CHECK: default {
// CHECK: node_self_Bases::E0

// `rb->rank = rb->rank + 1;`: a plain (non-enum) sibling-field mutation
// reached right after the B5 comparison and the enum-field write above —
// ordinary struct-member read/add/write, unaffected by the array-member
// pointer machinery.
// CHECK: %[[RANKWRITE:.*]] = emitrust.member %{{.*}}["rank"]
// CHECK: %[[RANKREAD:.*]] = emitrust.member %{{.*}}["rank"]
// CHECK: %[[RANKLOAD:.*]] = emitrust.load %[[RANKREAD]]
// CHECK: %[[ONE:.*]] = arith.constant 1 : i32
// CHECK: %[[RANKNEW:.*]] = arith.addi %[[RANKLOAD]], %[[ONE]] : i32
// CHECK: emitrust.assign %[[RANKWRITE]] = %[[RANKNEW]]

// The owner array itself is promoted to an array of the `node` struct.
// CHECK: emitrust.struct_def @Owner_main_arr ["data"] [!emitrust.array<4x!emitrust.struct<"node">>]

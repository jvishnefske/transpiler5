// RUN: emitrust-import-c %s | FileCheck %s

// Stage 4 of the owner-struct self-reference extension (design.md FR-30
// follow-on), import-only/FileCheck test — path compression (chained
// self-reference), the master plan's B3/B4 sub-blockers plus the loop
// comparison shape. Builds directly on Stage 2/3
// (`test/Import/C/array-self-ref-member.c` /
// `test/EndToEnd/array-self-ref-member.c`), which only proved the
// degenerate `x->self = x;` shape; this test proves the CHAINED shape
// `union-find.c`'s `uf_find` actually needs:
//   - B3: `parent = x->self;` — a LOCAL bound FROM a field READ. Fixed by
//     a new case in `PointerRegionAnalysis::recordPointerWrite`
//     (`lib/ImportC/ImportC.cpp`, gated by the new `arrayMemberFieldQuery`
//     callback) that joins the local into the arrow base's own class,
//     exactly like copying from the base directly.
//   - B4: `x->self = parent->self;` — a WRITE whose right-hand side is
//     ITSELF an array-member field read (not the cursor value itself, as
//     Stage 2 alone proved). Fixed in Pass A (`planArrayMemberPointers`,
//     `lib/ImportC/ImportCPlanning.cpp`): a right-hand side that is an
//     arrow read of a (candidate) array-member field is now as legal a
//     write source as the cursor value itself, checked via the SAME
//     read-side proof this pass runs for every site of the field. The
//     write-side EMISSION (`emitArrayMemberPointerAssign`) needed no
//     change at all: it already calls the general `emitPointerRValue` on
//     its right-hand side, which already dispatches an array-member field
//     read through `emitArrayMemberPointerRead` (Stage 2) regardless of
//     context.
//   - Loop comparison: `while (x->self != x)` decodes the field and
//     compares it against `x`'s own cursor. This turned out to need NO
//     dedicated special case: `emitArrayMemberPointerRead` already
//     returns `PtrExprValue{arrowBase->base, index}` (Stage 2), reusing
//     the arrow base's OWN resolved base identity — here literally `x`
//     itself on both sides of the comparison — so the general
//     pointer-equality machinery's `lhs->base == rhs->base` identity
//     check already passes trivially and the cursors compare correctly.
//     (A structural deviation from the plan, which anticipated needing a
//     dedicated comparison special case; verified unnecessary for this
//     shape by direct inspection of the emitted IR below.)

struct node { struct node *self; int rank; };

// An owner method binding one array element's `self` field to ANOTHER
// element (or itself): the right-hand side `target` is a second pointer
// PARAMETER of this same method, already proven usable by Stage 2's
// original rule (every data-pointer parameter of one method shares one
// class) — no Stage 4 change needed for this function.
void link_node(struct node *p, struct node *target) {
  p->self = target;
}

// `find`: the actual path-compression shape `union-find.c`'s `uf_find`
// needs. `parent = x->self;` is B3; `x->self = parent->self;` is B4;
// `while (x->self != x)` is the loop-comparison shape.
struct node *find(struct node *x) {
  struct node *parent;
  while (x->self != x) {
    parent = x->self;
    x->self = parent->self;
    x = parent;
  }
  return x;
}

int main(void) {
  struct node arr[4];
  // A chain: arr[0] is its own root; arr[1]->arr[0]; arr[2]->arr[1];
  // arr[3]->arr[2]. `find(&arr[3])` must compress this all the way to
  // arr[0] via B3/B4/loop-comparison.
  link_node(&arr[0], &arr[0]);
  link_node(&arr[1], &arr[0]);
  link_node(&arr[2], &arr[1]);
  link_node(&arr[3], &arr[2]);
  struct node *r = find(&arr[3]);
  return r->rank;
}

// The synthesized enum, one variant per array element, shared by every
// site of the field.
// CHECK: emitrust.enum_def @node_self_Bases ["E0", "E1", "E2", "E3"] [0, 1, 2, 3] {unsigned_underlying}
// CHECK: emitrust.struct_def @node ["self_", "rank"] [!emitrust.enum<"node_self_Bases">, i32]

// `link_node`: `p->self = target;` — Stage 2's original write shape,
// unchanged by Stage 4 (regression coverage that the new B4 fallback
// does not disturb the pre-existing rule).
// CHECK-LABEL: func.func @link_node
// CHECK-SAME: attributes {emitrust.method_of = "Owner_main_arr"
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

// `find`: the loop condition `x->self != x` decodes the field (branchless
// cast+widen, Stage 2) and compares it directly against `x`'s own loaded
// cursor with a plain `arith.cmpi ne` — no base-identity machinery.
// CHECK-LABEL: func.func @find
// CHECK-SAME: attributes {emitrust.method_of = "Owner_main_arr"
// CHECK: ^bb1
// CHECK: %[[FIELD:.*]] = emitrust.member %{{.*}}["self_"]
// CHECK: %[[LOADED:.*]] = emitrust.load %[[FIELD]]
// CHECK: %[[I32:.*]] = emitrust.cast %[[LOADED]] : !emitrust.enum<"node_self_Bases"> to i32
// CHECK: %[[WIDE:.*]] = arith.extsi %[[I32]] : i32 to i64
// CHECK: %[[XCUR:.*]] = memref.load %{{.*}}
// CHECK: arith.cmpi ne, %[[WIDE]], %[[XCUR]] : i64

// B3: `parent = x->self;` decodes the field into `parent`'s own cursor
// cell — no `emitrust.switch`, just the branchless decode followed by a
// plain store.
// CHECK: %[[PFIELD:.*]] = emitrust.member %{{.*}}["self_"]
// CHECK: %[[PLOADED:.*]] = emitrust.load %[[PFIELD]]
// CHECK: %[[PI32:.*]] = emitrust.cast %[[PLOADED]] : !emitrust.enum<"node_self_Bases"> to i32
// CHECK: %[[PWIDE:.*]] = arith.extsi %[[PI32]] : i32 to i64
// CHECK: memref.store %[[PWIDE]], %{{.*}}

// B4: `x->self = parent->self;` decodes `parent->self` (a second
// branchless decode chain) directly into the `emitrust.switch` encoding
// `x->self` — decode-then-encode of two plain i64s, no intermediate
// pointer-object machinery.
// CHECK: %[[RFIELD:.*]] = emitrust.member %{{.*}}["self_"]
// CHECK: %[[RLOADED:.*]] = emitrust.load %[[RFIELD]]
// CHECK: %[[RI32:.*]] = emitrust.cast %[[RLOADED]] : !emitrust.enum<"node_self_Bases"> to i32
// CHECK: %[[RWIDE:.*]] = arith.extsi %[[RI32]] : i32 to i64
// CHECK: emitrust.switch %[[RWIDE]] : i64
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

// The owner array itself is promoted to an array of the `node` struct.
// CHECK: emitrust.struct_def @Owner_main_arr ["data"] [!emitrust.array<4x!emitrust.struct<"node">>]

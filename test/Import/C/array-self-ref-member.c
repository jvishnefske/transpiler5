// RUN: emitrust-import-c %s | FileCheck %s

// Stage 2 of the owner-struct self-reference extension (design.md FR-30
// follow-on), import-only/FileCheck test (the differential end-to-end
// counterpart is Stage 3's `test/EndToEnd/array-self-ref-member.c`).
//
// `struct node`'s `self` field always points at an element of the SAME
// promoted owner array (`arr`, promoted by `planOwners` because it crosses
// a function boundary through `link_node`/`check_node`'s pointer
// parameters). A new whole-program Pass-A pass, `planArrayMemberPointers`
// (run right after `planOwners`), proves this closed-set property by
// walking every arrow access of the field and resolving each site's root
// through the same interprocedural machinery `planOwners` itself uses.
// Once proven:
//   - the field's MLIR type becomes a synthesized `emitrust.enum_def` (one
//     variant per array index) instead of a plain i64 cursor;
//   - every WRITE lowers to an `emitrust.switch` over the i64 index being
//     assigned — a genuine Rust `match`, not a bare transmute;
//   - every READ decodes with a plain enum-to-i32 cast (`castEnumToI32`)
//     widened to i64 — no branching, since the enum's storage IS the
//     index by construction.

struct node { struct node *self; int x; };

// An owner method: `p` is a promoted i64-index parameter into the (later
// promoted) local array `arr`. The arrow WRITE of `self` here roots in
// the same class as `p` itself, so Pass A proves the field usable.
void link_node(struct node *p) {
  p->self = p;
}

// A second owner method: the arrow READ of `self` also roots in the same
// class, validating the read side independently of the write.
int check_node(struct node *p) {
  return p->self == p;
}

int main(void) {
  struct node arr[2];
  link_node(&arr[0]);
  link_node(&arr[1]);
  int same = check_node(&arr[0]);
  return same;
}

// The synthesized enum, one variant per array element, module-scoped and
// created once (both link_node and check_node reuse this same symbol).
// CHECK: emitrust.enum_def @node_self_Bases ["E0", "E1"] [0, 1] {unsigned_underlying}

// The field's struct_def type is the enum, not a plain i64 cursor.
// CHECK: emitrust.struct_def @node ["self_", "x"] [!emitrust.enum<"node_self_Bases">, i32]

// CHECK-LABEL: func.func @link_node
// CHECK-SAME: attributes {emitrust.method_of = "Owner_main_arr"
//   The write lowers to a genuine match over the assigned i64 index: one
//   case per array element assigning that index's enum constant, plus a
//   default (provably unreachable) case assigning E0.
// CHECK: emitrust.switch %{{.*}} : i64
// CHECK-NEXT: case 0 {
// CHECK: emitrust.constant <#emitrust.opaque<"node_self_Bases::E0">> : !emitrust.enum<"node_self_Bases">
// CHECK: emitrust.assign
// CHECK: case 1 {
// CHECK: emitrust.constant <#emitrust.opaque<"node_self_Bases::E1">> : !emitrust.enum<"node_self_Bases">
// CHECK: emitrust.assign
// CHECK: default {
// CHECK: emitrust.constant <#emitrust.opaque<"node_self_Bases::E0">> : !emitrust.enum<"node_self_Bases">
// CHECK: emitrust.assign

// CHECK-LABEL: func.func @check_node
// CHECK-SAME: attributes {emitrust.method_of = "Owner_main_arr"
//   The read decodes with a plain cast (no branching) and widens to i64.
// CHECK: %[[FIELD:.*]] = emitrust.member %{{.*}}["self_"] : (!emitrust.lvalue<!emitrust.struct<"node">>) -> !emitrust.lvalue<!emitrust.enum<"node_self_Bases">>
// CHECK: %[[LOADED:.*]] = emitrust.load %[[FIELD]]
// CHECK: %[[I32:.*]] = emitrust.cast %[[LOADED]] : !emitrust.enum<"node_self_Bases"> to i32
// CHECK: arith.extsi %[[I32]] : i32 to i64
// CHECK: arith.cmpi eq

// The owner array itself is promoted to an array of the `node` struct.
// CHECK: emitrust.struct_def @Owner_main_arr ["data"] [!emitrust.array<2x!emitrust.struct<"node">>]

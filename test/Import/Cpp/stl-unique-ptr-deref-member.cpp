// RUN: emitrust-import-c %s | FileCheck %s
// FR-189: the NON-ARROW payload spelling `(*p).field` builds the SAME
// place as `p->field` -- one borrow of the Box, refined by
// `emitrust.deref` and projected by `emitrust.member` -- and never a
// staged COPY of the whole payload.
//
// This is the structural half of the pin; the LOAD-BEARING half is the
// runtime byte-diff in test/EndToEnd/stl-unique-ptr-deref-member.cpp,
// because a FileCheck of the IR cannot tell a silent miscompile from
// working code and this defect was exactly that. What the IR CAN pin,
// and what this file exists for, is the two structural properties whose
// absence caused it:
//
//  1. A WRITE takes `DerefMut::deref_mut` over an `addr_of mut`. Before
//     FR-189 the non-arrow spelling was not recognized as a payload
//     write at all, so it took the SHARED `Deref::deref` borrow.
//  2. NO `emitrust.load` of the whole payload struct appears anywhere in
//     these functions. The old lowering loaded the payload out of the
//     borrow into a fresh `emitrust.variable` and projected the member
//     of THAT -- which discarded writes silently, and which is rustc
//     E0507 on any non-`Copy` payload for reads. The CHECK-NOT lines are
//     the pin that the copy is gone.
//
// `matchStlBoxPayloadPlaceBase` (FR-188) is the single predicate behind
// both: FR-189 made `isStlBoxWriteExpr` defer to it rather than mint a
// second walker that could drift, so a chain this file admits and that
// walker rejects cannot arise.

#include <memory>

struct Inner {
  int x;
  int y;
};

struct Node {
  int id;
  int tag;
};

struct Outer {
  Inner a;
  int arr[4];
};

static int gi(const int &x) { return x + 1; }

// `(*p).id = a` is `p->id = a`: an `addr_of mut` of the Box, the
// `DerefMut` payload borrow, one `emitrust.member`, one `emitrust.assign`.
// CHECK-LABEL: func.func @star_write
// CHECK: %[[WB:.*]] = emitrust.addr_of mut %{{.*}} : (!emitrust.lvalue<!emitrust.opaque<"Box<Node>">>) -> !emitrust.mut_ref<!emitrust.opaque<"Box<Node>">>
// CHECK-NEXT: %[[WP:.*]] = emitrust.call_opaque "std::ops::DerefMut::deref_mut"(%[[WB]]) : (!emitrust.mut_ref<!emitrust.opaque<"Box<Node>">>) -> !emitrust.mut_ref<!emitrust.struct<"Node">>
// CHECK-NEXT: %[[WPL:.*]] = emitrust.deref %[[WP]] : (!emitrust.mut_ref<!emitrust.struct<"Node">>) -> !emitrust.lvalue<!emitrust.struct<"Node">>
// CHECK-NEXT: %[[WF:.*]] = emitrust.member %[[WPL]]["id"] : (!emitrust.lvalue<!emitrust.struct<"Node">>) -> !emitrust.lvalue<i32>
// CHECK-NEXT: emitrust.assign %[[WF]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK-NOT: emitrust.load %{{.*}} : (!emitrust.lvalue<!emitrust.struct<"Node">>) -> !emitrust.struct<"Node">
int star_write(int a) {
  auto p = std::make_unique<Node>();
  (*p).id = a;
  return 0;
}

// A non-arrow READ keeps the SHARED borrow (two payload reads in one
// expression would be rustc E0499 under a mutable one) and projects the
// FIELD out of it. The whole-payload load that used to sit between the
// deref and the member is what E0507 fired on.
// CHECK-LABEL: func.func @star_read
// CHECK: %[[RB:.*]] = emitrust.addr_of %{{.*}} : (!emitrust.lvalue<!emitrust.opaque<"Box<Node>">>) -> !emitrust.ref<!emitrust.opaque<"Box<Node>">>
// CHECK-NEXT: %[[RP:.*]] = emitrust.call_opaque "std::ops::Deref::deref"(%[[RB]]) : (!emitrust.ref<!emitrust.opaque<"Box<Node>">>) -> !emitrust.ref<!emitrust.struct<"Node">>
// CHECK-NEXT: %[[RPL:.*]] = emitrust.deref %[[RP]] : (!emitrust.ref<!emitrust.struct<"Node">>) -> !emitrust.lvalue<!emitrust.struct<"Node">>
// CHECK-NEXT: %[[RF:.*]] = emitrust.member %[[RPL]]["tag"] : (!emitrust.lvalue<!emitrust.struct<"Node">>) -> !emitrust.lvalue<i32>
// CHECK-NEXT: %[[RV:.*]] = emitrust.load %[[RF]] : (!emitrust.lvalue<i32>) -> i32
// CHECK-NOT: emitrust.load %{{.*}} : (!emitrust.lvalue<!emitrust.struct<"Node">>) -> !emitrust.struct<"Node">
int star_read(int a) {
  auto p = std::make_unique<Node>();
  p->id = a;
  return (*p).tag;
}

// The FR-188 positive leg one spelling over: a `const int &` argument out
// of `(*p).id` borrows the projected FIELD of the shared payload borrow.
// Its MUTABLE sibling `hi((*p).id)` stays a located rejection
// (stl-unique-ptr-invalid.cpp, FREEREFSTAR leg) -- FR-189 widened the
// PLACE, not the callee-driven borrow mutability.
// CHECK-LABEL: func.func @star_const_ref_field
// CHECK: %[[CB:.*]] = emitrust.addr_of %{{.*}} : (!emitrust.lvalue<!emitrust.opaque<"Box<Node>">>) -> !emitrust.ref<!emitrust.opaque<"Box<Node>">>
// CHECK-NEXT: %[[CP:.*]] = emitrust.call_opaque "std::ops::Deref::deref"(%[[CB]]) : (!emitrust.ref<!emitrust.opaque<"Box<Node>">>) -> !emitrust.ref<!emitrust.struct<"Node">>
// CHECK-NEXT: %[[CPL:.*]] = emitrust.deref %[[CP]] : (!emitrust.ref<!emitrust.struct<"Node">>) -> !emitrust.lvalue<!emitrust.struct<"Node">>
// CHECK-NEXT: %[[CF:.*]] = emitrust.member %[[CPL]]["id"] : (!emitrust.lvalue<!emitrust.struct<"Node">>) -> !emitrust.lvalue<i32>
// CHECK-NEXT: %[[CA:.*]] = emitrust.addr_of %[[CF]] : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
// CHECK-NEXT: %{{.*}} = call @gi(%[[CA]]) : (!emitrust.ref<i32>) -> i32
// CHECK-NOT: emitrust.load %{{.*}} : (!emitrust.lvalue<!emitrust.struct<"Node">>) -> !emitrust.struct<"Node">
int star_const_ref_field(int a) {
  auto p = std::make_unique<Node>();
  p->id = a;
  return gi((*p).id);
}

// The chains `matchStlBoxPayloadPlaceBase` walks: a NESTED member and a
// SUBSCRIPT, each rooted at the payload. Both must reach the MUTABLE
// borrow -- and the arrow sibling `q->a.y = a` in the middle pins that
// FR-189 fixed the nested ARROW form too, which used to take the shared
// borrow and surface as a deferred rustc E0594.
// CHECK-LABEL: func.func @nested_write
// CHECK: %[[NB:.*]] = emitrust.addr_of mut %{{.*}} : (!emitrust.lvalue<!emitrust.opaque<"Box<Outer>">>) -> !emitrust.mut_ref<!emitrust.opaque<"Box<Outer>">>
// CHECK-NEXT: %[[NP:.*]] = emitrust.call_opaque "std::ops::DerefMut::deref_mut"(%[[NB]]) : (!emitrust.mut_ref<!emitrust.opaque<"Box<Outer>">>) -> !emitrust.mut_ref<!emitrust.struct<"Outer">>
// CHECK-NEXT: %[[NPL:.*]] = emitrust.deref %[[NP]] : (!emitrust.mut_ref<!emitrust.struct<"Outer">>) -> !emitrust.lvalue<!emitrust.struct<"Outer">>
// CHECK-NEXT: %[[NA:.*]] = emitrust.member %[[NPL]]["a"] : (!emitrust.lvalue<!emitrust.struct<"Outer">>) -> !emitrust.lvalue<!emitrust.struct<"Inner">>
// CHECK-NEXT: %[[NX:.*]] = emitrust.member %[[NA]]["x"] : (!emitrust.lvalue<!emitrust.struct<"Inner">>) -> !emitrust.lvalue<i32>
// CHECK-NEXT: emitrust.assign %[[NX]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: %[[MB:.*]] = emitrust.addr_of mut %{{.*}} : (!emitrust.lvalue<!emitrust.opaque<"Box<Outer>">>) -> !emitrust.mut_ref<!emitrust.opaque<"Box<Outer>">>
// CHECK-NEXT: %[[MP:.*]] = emitrust.call_opaque "std::ops::DerefMut::deref_mut"(%[[MB]]) : (!emitrust.mut_ref<!emitrust.opaque<"Box<Outer>">>) -> !emitrust.mut_ref<!emitrust.struct<"Outer">>
// CHECK-NEXT: %[[MPL:.*]] = emitrust.deref %[[MP]] : (!emitrust.mut_ref<!emitrust.struct<"Outer">>) -> !emitrust.lvalue<!emitrust.struct<"Outer">>
// CHECK-NEXT: %[[MA:.*]] = emitrust.member %[[MPL]]["a"] : (!emitrust.lvalue<!emitrust.struct<"Outer">>) -> !emitrust.lvalue<!emitrust.struct<"Inner">>
// CHECK-NEXT: %[[MY:.*]] = emitrust.member %[[MA]]["y"] : (!emitrust.lvalue<!emitrust.struct<"Inner">>) -> !emitrust.lvalue<i32>
// CHECK-NEXT: emitrust.assign %[[MY]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: %[[SB:.*]] = emitrust.addr_of mut %{{.*}} : (!emitrust.lvalue<!emitrust.opaque<"Box<Outer>">>) -> !emitrust.mut_ref<!emitrust.opaque<"Box<Outer>">>
// CHECK-NEXT: %[[SP:.*]] = emitrust.call_opaque "std::ops::DerefMut::deref_mut"(%[[SB]]) : (!emitrust.mut_ref<!emitrust.opaque<"Box<Outer>">>) -> !emitrust.mut_ref<!emitrust.struct<"Outer">>
// CHECK-NEXT: %[[SPL:.*]] = emitrust.deref %[[SP]] : (!emitrust.mut_ref<!emitrust.struct<"Outer">>) -> !emitrust.lvalue<!emitrust.struct<"Outer">>
// CHECK-NEXT: %[[SARR:.*]] = emitrust.member %[[SPL]]["arr"] : (!emitrust.lvalue<!emitrust.struct<"Outer">>) -> !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK: %[[SEL:.*]] = emitrust.subscript %[[SARR]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
// CHECK-NEXT: emitrust.assign %[[SEL]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK-NOT: emitrust.load %{{.*}} : (!emitrust.lvalue<!emitrust.struct<"Outer">>) -> !emitrust.struct<"Outer">
int nested_write(int a) {
  auto q = std::make_unique<Outer>();
  (*q).a.x = a;
  q->a.y = a;
  (*q).arr[1] = a;
  return 0;
}

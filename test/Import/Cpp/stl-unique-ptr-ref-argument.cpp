// RUN: emitrust-import-c %s | FileCheck %s
// FR-188: the OVER-REJECTION GUARD for the mutable-reference-argument
// rejection pinned in stl-unique-ptr-invalid.cpp.
//
// FR-188 rejects a `&mut` argument bound to a std::unique_ptr payload
// place, because the payload borrow a read takes is the SHARED
// `Deref::deref` one and a mutable re-borrow out of it is rustc E0596
// (and, for the `(*p).field` spelling, a silent write-to-a-copy
// miscompile). The rejection is keyed on the PARAMETER's mutability, and
// this file exists because keying it on anything coarser -- "the argument
// is a payload place", "the call has a reference parameter" -- would
// silently stop importing the three shapes below, every one of which is
// correct TODAY and stays correct.
//
// What each shape pins, and why the shared borrow is right for it:
//  - `const Node &`: C++ promises the callee will not write, so ONE
//    shared borrow of the payload is the faithful image. This is the
//    exact sibling of the rejected `Node &` and the single most likely
//    thing a too-broad predicate would break.
//  - `const int &` over `p->field`: the same, one field projection
//    deeper, so a predicate that walks member bases still has to stop at
//    the parameter's constness rather than at the walk succeeding.
//  - a BY-VALUE payload parameter: no borrow is taken at all, the payload
//    is loaded and copied, and the callee's `&mut`-ness cannot arise.
//
// The last function pins that FR-188 did not disturb mutable reference
// arguments generally: a `&mut` to an ordinary local is untouched, and it
// sits in the same file as the rejected shapes so the two cannot drift
// apart. The runtime half of this guard -- that these still BUILD and
// still print what clang++ prints -- is in
// test/EndToEnd/stl-unique-ptr.cpp's `refargs`, because a FileCheck of
// the IR cannot see a miscompile.

#include <memory>

struct Node {
  int id;
  int tag;
};

static int g(const Node &n) { return n.id + n.tag; }
static int gi(const int &x) { return x + 1; }
static int byval(Node n) { return n.id + n.tag; }
static void bump(int &x) { x += 1; }

// A `const Node &` parameter takes the SHARED payload borrow and then a
// SHARED `addr_of` of the payload place -- no `DerefMut`, no `addr_of
// mut` anywhere in the argument.
// CHECK-LABEL: func.func @const_ref
// CHECK: %[[BORROW:.*]] = emitrust.addr_of %{{.*}} : (!emitrust.lvalue<!emitrust.opaque<"Box<Node>">>) -> !emitrust.ref<!emitrust.opaque<"Box<Node>">>
// CHECK-NEXT: %[[PAYLOAD:.*]] = emitrust.call_opaque "std::ops::Deref::deref"(%[[BORROW]]) : (!emitrust.ref<!emitrust.opaque<"Box<Node>">>) -> !emitrust.ref<!emitrust.struct<"Node">>
// CHECK-NEXT: %[[PLACE:.*]] = emitrust.deref %[[PAYLOAD]] : (!emitrust.ref<!emitrust.struct<"Node">>) -> !emitrust.lvalue<!emitrust.struct<"Node">>
// CHECK-NEXT: %[[ARG:.*]] = emitrust.addr_of %[[PLACE]] : (!emitrust.lvalue<!emitrust.struct<"Node">>) -> !emitrust.ref<!emitrust.struct<"Node">>
// CHECK-NEXT: %{{.*}} = call @g(%[[ARG]]) : (!emitrust.ref<!emitrust.struct<"Node">>) -> i32
int const_ref(int a) {
  auto p = std::make_unique<Node>();
  p->id = a;
  return g(*p);
}

// `p->field` on a `const int &` parameter: the same shared chain with an
// `emitrust.member` projection before the borrow.
// CHECK-LABEL: func.func @const_ref_field
// CHECK: %[[FBORROW:.*]] = emitrust.addr_of %{{.*}} : (!emitrust.lvalue<!emitrust.opaque<"Box<Node>">>) -> !emitrust.ref<!emitrust.opaque<"Box<Node>">>
// CHECK-NEXT: %[[FPAYLOAD:.*]] = emitrust.call_opaque "std::ops::Deref::deref"(%[[FBORROW]]) : (!emitrust.ref<!emitrust.opaque<"Box<Node>">>) -> !emitrust.ref<!emitrust.struct<"Node">>
// CHECK-NEXT: %[[FPLACE:.*]] = emitrust.deref %[[FPAYLOAD]] : (!emitrust.ref<!emitrust.struct<"Node">>) -> !emitrust.lvalue<!emitrust.struct<"Node">>
// CHECK-NEXT: %[[FIELD:.*]] = emitrust.member %[[FPLACE]]["id"] : (!emitrust.lvalue<!emitrust.struct<"Node">>) -> !emitrust.lvalue<i32>
// CHECK-NEXT: %[[FARG:.*]] = emitrust.addr_of %[[FIELD]] : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
// CHECK-NEXT: %{{.*}} = call @gi(%[[FARG]]) : (!emitrust.ref<i32>) -> i32
int const_ref_field(int a) {
  auto p = std::make_unique<Node>();
  p->id = a;
  return gi(p->id);
}

// A by-VALUE payload parameter borrows nothing: the payload is LOADED
// through the shared borrow and passed as a struct value.
// CHECK-LABEL: func.func @by_value
// CHECK: %[[VBORROW:.*]] = emitrust.addr_of %{{.*}} : (!emitrust.lvalue<!emitrust.opaque<"Box<Node>">>) -> !emitrust.ref<!emitrust.opaque<"Box<Node>">>
// CHECK-NEXT: %[[VPAYLOAD:.*]] = emitrust.call_opaque "std::ops::Deref::deref"(%[[VBORROW]]) : (!emitrust.ref<!emitrust.opaque<"Box<Node>">>) -> !emitrust.ref<!emitrust.struct<"Node">>
// CHECK-NEXT: %[[VPLACE:.*]] = emitrust.deref %[[VPAYLOAD]] : (!emitrust.ref<!emitrust.struct<"Node">>) -> !emitrust.lvalue<!emitrust.struct<"Node">>
// CHECK-NEXT: %[[VVAL:.*]] = emitrust.load %[[VPLACE]] : (!emitrust.lvalue<!emitrust.struct<"Node">>) -> !emitrust.struct<"Node">
// CHECK-NEXT: %{{.*}} = call @byval(%[[VVAL]]) : (!emitrust.struct<"Node">) -> i32
int by_value(int a) {
  auto p = std::make_unique<Node>();
  p->id = a;
  return byval(*p);
}

// FR-188 narrowed nothing outside the payload: a `&mut` argument to an
// ordinary local still emits the plain mutable borrow it always did.
// CHECK-LABEL: func.func @plain_mut_ref
// CHECK: %[[LOCAL:.*]] = emitrust.variable named "x" : !emitrust.lvalue<i32>
// CHECK: %[[MUTARG:.*]] = emitrust.addr_of mut %[[LOCAL]] : (!emitrust.lvalue<i32>) -> !emitrust.mut_ref<i32>
// CHECK-NEXT: call @bump(%[[MUTARG]]) : (!emitrust.mut_ref<i32>) -> ()
int plain_mut_ref(int a) {
  int x = a;
  bump(x);
  return x;
}

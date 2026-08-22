// RUN: emitrust-import-c %s | FileCheck %s
// W2.21: std::unique_ptr<T> imports as !emitrust.opaque<"Box<T>"> and
// std::make_unique<T>(args) as Box::new — Rust's Box IS single ownership,
// so no synthesized struct and no new dialect op are needed.
//
// BARE Box, not Option<Box>: both images were measured byte-identical
// against clang++ -std=c++17 during the W2.21 spike, so the choice is
// cost and not correctness, and Option<Box<T>> would force an unwrap at
// every dereference an always-initialized program never needed. This
// follows FR-99's precedent verbatim ("the local stays the bare struct");
// every nullable spelling is a located rejection pinned in
// stl-unique-ptr-invalid.cpp.
//
// This file pins the chosen lowerings at the IR level, because each one
// is a decision another shape could plausibly have taken:
//  - a SCALAR payload is one `call_opaque "Box::new"(v)`, and
//    make_unique<int>() with no argument value-initializes, so it boxes a
//    zero constant;
//  - a STRUCT payload is the W2.17 TWO-STEP: `Box::new(T::default())`
//    establishes the storage, then the C++ constructor runs as an
//    ordinary `&mut self` method ON THE BOX PLACE. No `T::default()`
//    payload is ever dropped, because Box::new MOVES it;
//  - `p->m()` / `(*p).m()` are `emitrust.method_call` DIRECTLY on the Box
//    place (Rust auto-deref renders `p.node_bump(2i32)`), NOT a call
//    through the payload borrow. That split is load-bearing: a borrowed
//    receiver is built BEFORE the argument values, so `p->bump(p->get())`
//    would be rustc E0502;
//  - `p->field` and `*n` go through `addr_of` + a UFCS
//    `std::ops::Deref::deref` + `emitrust.deref`, because
//    `emitrust.member` requires a StructType lvalue and `emitrust.deref`
//    only accepts a ref/mut_ref. READS take the SHARED borrow (two `&mut`
//    borrows of one Box live at once — two field reads in one printf —
//    is rustc E0499) and only the three WRITE positions take
//    `DerefMut::deref_mut`.

extern "C" int printf(const char *, ...);

#include <memory>

struct Node {
  int id;
  int tag;
  Node(int i, int t) : id(i), tag(t) { printf("ctor %d/%d\n", id, tag); }
  void bump(int d) { id += d; }
  int get() const { return id; }
  ~Node() { printf("dtor %d\n", id); }
};

// CHECK-LABEL: func.func @use_scalar
// A scalar payload: one Box::new over the argument value. The
// forwarding-reference parameter applies no conversion, so the argument
// arrives at its own type and is boxed as-is.
// CHECK: %[[N:.*]] = emitrust.variable named "n" : !emitrust.lvalue<!emitrust.opaque<"Box<i32>">>
// CHECK: %[[BOX:.*]] = emitrust.call_opaque "Box::new"(%{{.*}}) : (i32) -> !emitrust.opaque<"Box<i32>">
// CHECK: emitrust.assign %[[N]] = %[[BOX]] : !emitrust.lvalue<!emitrust.opaque<"Box<i32>">>
// The READ place: a SHARED borrow through Deref::deref, then a deref.
// CHECK: %[[R0:.*]] = emitrust.addr_of %[[N]] : (!emitrust.lvalue<!emitrust.opaque<"Box<i32>">>) -> !emitrust.ref<!emitrust.opaque<"Box<i32>">>
// CHECK: %[[D0:.*]] = emitrust.call_opaque "std::ops::Deref::deref"(%[[R0]]) : (!emitrust.ref<!emitrust.opaque<"Box<i32>">>) -> !emitrust.ref<i32>
// CHECK: %[[P0:.*]] = emitrust.deref %[[D0]] : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
// CHECK: emitrust.load %[[P0]]
// The WRITE place of `*n = ...`: the MUTABLE borrow, and the value is
// already computed (C++17 P0145R3 right-hand-side-first, which is also
// what keeps the read borrow dead before the write borrow is taken).
// CHECK: %[[RM:.*]] = emitrust.addr_of mut %[[N]] : (!emitrust.lvalue<!emitrust.opaque<"Box<i32>">>) -> !emitrust.mut_ref<!emitrust.opaque<"Box<i32>">>
// CHECK: %[[DM:.*]] = emitrust.call_opaque "std::ops::DerefMut::deref_mut"(%[[RM]]) : (!emitrust.mut_ref<!emitrust.opaque<"Box<i32>">>) -> !emitrust.mut_ref<i32>
// CHECK: %[[PM:.*]] = emitrust.deref %[[DM]] : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[PM]] = %{{.*}} : !emitrust.lvalue<i32>
int use_scalar(int seed) {
  auto n = std::make_unique<int>(seed);
  int read = *n;
  *n = read + 1;
  return *n;
}

// CHECK-LABEL: func.func @use_zero
// `std::make_unique<int>()` VALUE-initializes, so the zero-argument
// spelling boxes a zero rather than leaving the storage undefined.
// CHECK: %[[Z:.*]] = arith.constant 0 : i32
// CHECK: emitrust.call_opaque "Box::new"(%[[Z]]) : (i32) -> !emitrust.opaque<"Box<i32>">
int use_zero(void) {
  auto z = std::make_unique<int>();
  return *z;
}

// CHECK-LABEL: func.func @use_struct
// The struct payload's TWO-STEP construction.
// CHECK: %[[P:.*]] = emitrust.variable named "p" : !emitrust.lvalue<!emitrust.opaque<"Box<Node>">>
// CHECK: %[[DEF:.*]] = emitrust.call_opaque "Node::default"() : () -> !emitrust.struct<"Node">
// CHECK: %[[PB:.*]] = emitrust.call_opaque "Box::new"(%[[DEF]]) : (!emitrust.struct<"Node">) -> !emitrust.opaque<"Box<Node>">
// CHECK: emitrust.assign %[[P]] = %[[PB]] : !emitrust.lvalue<!emitrust.opaque<"Box<Node>">>
// CHECK: emitrust.method_call %[[P]]["Node_new"] (%{{.*}}, %{{.*}}) : (!emitrust.lvalue<!emitrust.opaque<"Box<Node>">>, i32, i32) -> ()
// `p->field` reads: the SHARED borrow refined into a struct place, then
// an ordinary member projection. Two of them live at once in one printf.
// CHECK: %[[FR:.*]] = emitrust.addr_of %[[P]] : (!emitrust.lvalue<!emitrust.opaque<"Box<Node>">>) -> !emitrust.ref<!emitrust.opaque<"Box<Node>">>
// CHECK: %[[FD:.*]] = emitrust.call_opaque "std::ops::Deref::deref"(%[[FR]]) : (!emitrust.ref<!emitrust.opaque<"Box<Node>">>) -> !emitrust.ref<!emitrust.struct<"Node">>
// CHECK: %[[FP:.*]] = emitrust.deref %[[FD]] : (!emitrust.ref<!emitrust.struct<"Node">>) -> !emitrust.lvalue<!emitrust.struct<"Node">>
// CHECK: emitrust.member %[[FP]]["id"]
// `p->m()` / `(*p).m()`: a method_call on the BOX place, never on a
// borrow of the payload.
// CHECK: emitrust.method_call %[[P]]["Node_bump"] (%{{.*}}) : (!emitrust.lvalue<!emitrust.opaque<"Box<Node>">>, i32) -> ()
// CHECK: emitrust.method_call %[[P]]["Node_get"] () : (!emitrust.lvalue<!emitrust.opaque<"Box<Node>">>) -> i32
// The field WRITE: the value first (P0145R3 and E0502), then the MUTABLE
// borrow, then the member place.
// CHECK: %[[WR:.*]] = emitrust.addr_of mut %[[P]] : (!emitrust.lvalue<!emitrust.opaque<"Box<Node>">>) -> !emitrust.mut_ref<!emitrust.opaque<"Box<Node>">>
// CHECK: %[[WD:.*]] = emitrust.call_opaque "std::ops::DerefMut::deref_mut"(%[[WR]]) : (!emitrust.mut_ref<!emitrust.opaque<"Box<Node>">>) -> !emitrust.mut_ref<!emitrust.struct<"Node">>
// CHECK: %[[WP:.*]] = emitrust.deref %[[WD]] : (!emitrust.mut_ref<!emitrust.struct<"Node">>) -> !emitrust.lvalue<!emitrust.struct<"Node">>
// CHECK: %[[WM:.*]] = emitrust.member %[[WP]]["id"]
// CHECK: emitrust.assign %[[WM]] = %{{.*}} : !emitrust.lvalue<i32>
int use_struct(int seed) {
  auto p = std::make_unique<Node>(seed + 10, seed);
  printf("field=%d tag=%d\n", p->id, p->tag);
  p->bump(2);
  int got = p->get();
  (*p).bump(3);
  p->id = got + 1;
  return (*p).get();
}

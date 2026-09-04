// FR-190: a `std::unique_ptr` payload access imports as the op TRIPLE
// `emitrust.addr_of` -> `emitrust.call_opaque "std::ops::Deref::deref"` ->
// `emitrust.deref`, which used to render as three Rust lines ending in an
// explicit trait call (`let v7: &i32 = std::ops::Deref::deref(v6);`).
// Idiomatic Rust spells that place `*n`, and clippy::borrowed_box flags the
// `&Box<T>` binding the old spelling forces. This file pins the EMISSION-SIDE
// fold: the IR is untouched (no dialect change, no importer change), the two
// producer ops simply print NOTHING at their program point and every consumer
// renders the Box place itself.
//
// Each function below pins one constraint the spike measured as load-bearing.
// The fold is a RENDERING change and the byte-diff oracle
// (test/EndToEnd/stl-unique-ptr.cpp, test/EndToEnd/prelude-shadow-box.cpp)
// is what proves it behavior-preserving; this file is what keeps the SHAPE
// of the fold from silently widening.
//
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

emitrust.struct_def @Pod ["a", "b"] [i32, i32]
emitrust.struct_def @Owner ["id"] [i32] {emitrust.has_drop}

// 1. THE PLAIN FOLD. `*std::ops::Deref::deref(&n)` is `*n`. Neither the
// borrow nor the trait call may appear anywhere in the body.
// CHECK-LABEL: fn fold_plain(
// CHECK-NOT:     Deref::deref
// CHECK-NOT:     &Box<i32>
// CHECK:         println!("{}", *n);
emitrust.func @fold_plain(%arg0: i32) {
  %b = emitrust.variable named "n" : !emitrust.lvalue<!emitrust.opaque<"Box<i32>">>
  %mk = emitrust.call_opaque "Box::new"(%arg0) : (i32) -> !emitrust.opaque<"Box<i32>">
  emitrust.assign %b = %mk : !emitrust.lvalue<!emitrust.opaque<"Box<i32>">>
  %r = emitrust.addr_of %b : (!emitrust.lvalue<!emitrust.opaque<"Box<i32>">>) -> !emitrust.ref<!emitrust.opaque<"Box<i32>">>
  %c = emitrust.call_opaque "std::ops::Deref::deref"(%r) : (!emitrust.ref<!emitrust.opaque<"Box<i32>">>) -> !emitrust.ref<i32>
  %p = emitrust.deref %c : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
  %v = emitrust.load %p : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "println!"(%v) {args = ["{}", 0 : index]} : (i32) -> ()
  emitrust.return
}

// 2. THE MEMBER FORM. Under a `.field` projection the fold is `P.f`, not
// `(*P).f`: Rust's auto-deref reaches through the Box, and the parenthesized
// spelling is what clippy::explicit_auto_deref rejects (the same rule the
// emitter already applied to a reference base).
// CHECK-LABEL: fn fold_member(
// CHECK-NOT:     Deref::deref
// CHECK:         println!("{}", p.a);
emitrust.func @fold_member() {
  %b = emitrust.variable named "p" : !emitrust.lvalue<!emitrust.opaque<"Box<Pod>">>
  %r = emitrust.addr_of %b : (!emitrust.lvalue<!emitrust.opaque<"Box<Pod>">>) -> !emitrust.ref<!emitrust.opaque<"Box<Pod>">>
  %c = emitrust.call_opaque "std::ops::Deref::deref"(%r) : (!emitrust.ref<!emitrust.opaque<"Box<Pod>">>) -> !emitrust.ref<!emitrust.struct<"Pod">>
  %p = emitrust.deref %c : (!emitrust.ref<!emitrust.struct<"Pod">>) -> !emitrust.lvalue<!emitrust.struct<"Pod">>
  %f = emitrust.member %p["a"] : (!emitrust.lvalue<!emitrust.struct<"Pod">>) -> !emitrust.lvalue<i32>
  %v = emitrust.load %f : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "println!"(%v) {args = ["{}", 0 : index]} : (i32) -> ()
  emitrust.return
}

// 3. THE BORROW FORMS, and constraint 4 with them. A borrow of the folded
// place is `&*p` / `&mut *p` -- NOT `&p` / `&mut p`. Deref coercion would
// make the shorter spelling compile, but it is fragile and buys no lint:
// clippy::borrow_deref_ref fires on `&*x` only when `x` is ALREADY a
// reference, which is exactly what the OLD spelling produced and the fold
// removes.
// CHECK-LABEL: fn fold_borrows(
// CHECK-NOT:     Deref::deref
// CHECK-NOT:     DerefMut::deref_mut
// CHECK:         let [[SH:v[0-9]+]]: &i32 = &*q;
// CHECK:         take_shared([[SH]]);
// CHECK:         let [[MU:v[0-9]+]]: &mut i32 = &mut *q;
// CHECK:         take_mut([[MU]]);
emitrust.func @fold_borrows() {
  %b = emitrust.variable named "q" : !emitrust.lvalue<!emitrust.opaque<"Box<i32>">>
  %r = emitrust.addr_of %b : (!emitrust.lvalue<!emitrust.opaque<"Box<i32>">>) -> !emitrust.ref<!emitrust.opaque<"Box<i32>">>
  %c = emitrust.call_opaque "std::ops::Deref::deref"(%r) : (!emitrust.ref<!emitrust.opaque<"Box<i32>">>) -> !emitrust.ref<i32>
  %p = emitrust.deref %c : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
  %sh = emitrust.addr_of %p : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
  emitrust.call_opaque "take_shared"(%sh) : (!emitrust.ref<i32>) -> ()
  %rm = emitrust.addr_of mut %b : (!emitrust.lvalue<!emitrust.opaque<"Box<i32>">>) -> !emitrust.mut_ref<!emitrust.opaque<"Box<i32>">>
  %cm = emitrust.call_opaque "std::ops::DerefMut::deref_mut"(%rm) : (!emitrust.mut_ref<!emitrust.opaque<"Box<i32>">>) -> !emitrust.mut_ref<i32>
  %pm = emitrust.deref %cm : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
  %mu = emitrust.addr_of mut %pm : (!emitrust.lvalue<i32>) -> !emitrust.mut_ref<i32>
  emitrust.call_opaque "take_mut"(%mu) : (!emitrust.mut_ref<i32>) -> ()
  emitrust.return
}

// 4. `let mut` SURVIVES THE FOLD. The `emitrust.addr_of mut` stays in the IR
// precisely so `computeDeferredInits`'s `postInitMutation` scan still sees the
// mutable borrow of the Box; suppressing the OP instead of the TEXT would drop
// the `mut` and the emitted crate's own `deny` settings would reject
// `*n = ..` on an immutable binding (rustc E0596). The same op is what keeps
// `bindingIsBorrowed` true, holding a Drop-carrying Box out of loop-body
// dead-store elision.
// CHECK-LABEL: fn fold_keeps_mut(
// CHECK:         let mut m: Box<i32>;
// CHECK-NOT:     DerefMut::deref_mut
// CHECK:         *m = {{.*}};
emitrust.func @fold_keeps_mut(%arg0: i32) {
  %b = emitrust.variable named "m" : !emitrust.lvalue<!emitrust.opaque<"Box<i32>">>
  %mk = emitrust.call_opaque "Box::new"(%arg0) : (i32) -> !emitrust.opaque<"Box<i32>">
  emitrust.assign %b = %mk : !emitrust.lvalue<!emitrust.opaque<"Box<i32>">>
  %rm = emitrust.addr_of mut %b : (!emitrust.lvalue<!emitrust.opaque<"Box<i32>">>) -> !emitrust.mut_ref<!emitrust.opaque<"Box<i32>">>
  %cm = emitrust.call_opaque "std::ops::DerefMut::deref_mut"(%rm) : (!emitrust.mut_ref<!emitrust.opaque<"Box<i32>">>) -> !emitrust.mut_ref<i32>
  %pm = emitrust.deref %cm : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
  %one = emitrust.constant <1 : i32> : i32
  emitrust.assign %pm = %one : !emitrust.lvalue<i32>
  %v = emitrust.load %pm : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "println!"(%v) {args = ["{}", 0 : index]} : (i32) -> ()
  emitrust.return
}

// 5. CONSTRAINT 3, first half: a MULTI-USE `addr_of` does not fold. Two trait
// calls share one borrow, so suppressing the borrow's text would orphan the
// second call's operand (rustc E0425). Today's spelling is kept for the whole
// chain.
// CHECK-LABEL: fn no_fold_shared_borrow(
// CHECK:         let [[B:v[0-9]+]]: &Box<i32> = &s;
// CHECK:         let [[A:v[0-9]+]]: &i32 = std::ops::Deref::deref([[B]]);
// CHECK:         let [[C:v[0-9]+]]: &i32 = std::ops::Deref::deref([[B]]);
emitrust.func @no_fold_shared_borrow() {
  %b = emitrust.variable named "s" : !emitrust.lvalue<!emitrust.opaque<"Box<i32>">>
  %r = emitrust.addr_of %b : (!emitrust.lvalue<!emitrust.opaque<"Box<i32>">>) -> !emitrust.ref<!emitrust.opaque<"Box<i32>">>
  %c1 = emitrust.call_opaque "std::ops::Deref::deref"(%r) : (!emitrust.ref<!emitrust.opaque<"Box<i32>">>) -> !emitrust.ref<i32>
  %c2 = emitrust.call_opaque "std::ops::Deref::deref"(%r) : (!emitrust.ref<!emitrust.opaque<"Box<i32>">>) -> !emitrust.ref<i32>
  %p1 = emitrust.deref %c1 : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
  %p2 = emitrust.deref %c2 : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
  %v1 = emitrust.load %p1 : (!emitrust.lvalue<i32>) -> i32
  %v2 = emitrust.load %p2 : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "println!"(%v1, %v2) {args = ["{} {}", 0 : index, 1 : index]} : (i32, i32) -> ()
  emitrust.return
}

// 6. CONSTRAINT 3, second half: a multi-use CALL RESULT does not fold either.
// CHECK-LABEL: fn no_fold_shared_call(
// CHECK:         let [[B2:v[0-9]+]]: &Box<i32> = &t;
// CHECK:         let [[A2:v[0-9]+]]: &i32 = std::ops::Deref::deref([[B2]]);
emitrust.func @no_fold_shared_call() {
  %b = emitrust.variable named "t" : !emitrust.lvalue<!emitrust.opaque<"Box<i32>">>
  %r = emitrust.addr_of %b : (!emitrust.lvalue<!emitrust.opaque<"Box<i32>">>) -> !emitrust.ref<!emitrust.opaque<"Box<i32>">>
  %c = emitrust.call_opaque "std::ops::Deref::deref"(%r) : (!emitrust.ref<!emitrust.opaque<"Box<i32>">>) -> !emitrust.ref<i32>
  %p1 = emitrust.deref %c : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
  %p2 = emitrust.deref %c : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
  %v1 = emitrust.load %p1 : (!emitrust.lvalue<i32>) -> i32
  %v2 = emitrust.load %p2 : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "println!"(%v1, %v2) {args = ["{} {}", 0 : index, 1 : index]} : (i32, i32) -> ()
  emitrust.return
}

// 7. CONSTRAINT 1, THE ONE THAT PROTECTS A DESTRUCTOR. A whole-value load of a
// NON-`Copy` payload must keep the trait call. `*Deref::deref(&p)` can never
// move -- rustc rejects it with a loud E0507 -- but `*p` on a `Box` CAN, and a
// move out of the Box deinitialises it and SKIPS its `Drop`. Folding here
// would trade a compile error for a silently missing destructor line, the
// direction this project forbids.
// CHECK-LABEL: fn no_fold_noncopy_load(
// CHECK:         let [[BO:v[0-9]+]]: &Box<Owner> = &o;
// CHECK:         let {{v[0-9]+}}: &Owner = std::ops::Deref::deref([[BO]]);
emitrust.func @no_fold_noncopy_load() {
  %b = emitrust.variable named "o" : !emitrust.lvalue<!emitrust.opaque<"Box<Owner>">>
  %r = emitrust.addr_of %b : (!emitrust.lvalue<!emitrust.opaque<"Box<Owner>">>) -> !emitrust.ref<!emitrust.opaque<"Box<Owner>">>
  %c = emitrust.call_opaque "std::ops::Deref::deref"(%r) : (!emitrust.ref<!emitrust.opaque<"Box<Owner>">>) -> !emitrust.ref<!emitrust.struct<"Owner">>
  %p = emitrust.deref %c : (!emitrust.ref<!emitrust.struct<"Owner">>) -> !emitrust.lvalue<!emitrust.struct<"Owner">>
  %v = emitrust.load %p : (!emitrust.lvalue<!emitrust.struct<"Owner">>) -> !emitrust.struct<"Owner">
  %d = emitrust.variable named "copy" : !emitrust.lvalue<!emitrust.struct<"Owner">>
  emitrust.assign %d = %v : !emitrust.lvalue<!emitrust.struct<"Owner">>
  %f = emitrust.member %d["id"] : (!emitrust.lvalue<!emitrust.struct<"Owner">>) -> !emitrust.lvalue<i32>
  %i = emitrust.load %f : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "println!"(%i) {args = ["{}", 0 : index]} : (i32) -> ()
  emitrust.return
}

// 7b. The same non-`Copy` payload under a FIELD projection still folds: a
// `.field` read copies the field, it does not move the payload, so no Drop is
// at risk. The gate is the whole-value load, not the payload type.
// CHECK-LABEL: fn fold_noncopy_member(
// CHECK-NOT:     Deref::deref
// CHECK:         println!("{}", w.id);
emitrust.func @fold_noncopy_member() {
  %b = emitrust.variable named "w" : !emitrust.lvalue<!emitrust.opaque<"Box<Owner>">>
  %r = emitrust.addr_of %b : (!emitrust.lvalue<!emitrust.opaque<"Box<Owner>">>) -> !emitrust.ref<!emitrust.opaque<"Box<Owner>">>
  %c = emitrust.call_opaque "std::ops::Deref::deref"(%r) : (!emitrust.ref<!emitrust.opaque<"Box<Owner>">>) -> !emitrust.ref<!emitrust.struct<"Owner">>
  %p = emitrust.deref %c : (!emitrust.ref<!emitrust.struct<"Owner">>) -> !emitrust.lvalue<!emitrust.struct<"Owner">>
  %f = emitrust.member %p["id"] : (!emitrust.lvalue<!emitrust.struct<"Owner">>) -> !emitrust.lvalue<i32>
  %v = emitrust.load %f : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "println!"(%v) {args = ["{}", 0 : index]} : (i32) -> ()
  emitrust.return
}

// 8. CONSTRAINT 5: the SIBLING `std::ops::Index::index` fold (W2.20's
// `m.at(k)` place) is a different callee with two operands and zero measured
// clippy warnings. It is a separate FR and must be untouched here.
// CHECK-LABEL: fn index_untouched(
// CHECK:         let [[MR:v[0-9]+]]: &BTreeMap<i32, i32> = &m;
// CHECK:         let [[KR:v[0-9]+]]: &i32 = &k;
// CHECK:         let [[IX:v[0-9]+]]: &i32 = std::ops::Index::index([[MR]], [[KR]]);
// CHECK:         println!("{}", *[[IX]]);
emitrust.func @index_untouched() {
  %m = emitrust.variable named "m" : !emitrust.lvalue<!emitrust.opaque<"BTreeMap<i32, i32>">>
  %k = emitrust.variable named "k" : !emitrust.lvalue<i32>
  %mr = emitrust.addr_of %m : (!emitrust.lvalue<!emitrust.opaque<"BTreeMap<i32, i32>">>) -> !emitrust.ref<!emitrust.opaque<"BTreeMap<i32, i32>">>
  %kr = emitrust.addr_of %k : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
  %ix = emitrust.call_opaque "std::ops::Index::index"(%mr, %kr) : (!emitrust.ref<!emitrust.opaque<"BTreeMap<i32, i32>">>, !emitrust.ref<i32>) -> !emitrust.ref<i32>
  %ap = emitrust.deref %ix : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
  %v = emitrust.load %ap : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "println!"(%v) {args = ["{}", 0 : index]} : (i32) -> ()
  emitrust.return
}

// 9. A PLAIN REFERENCE deref is not a Box fold and keeps rendering `*x`
// through its own operand -- the fold matcher must key on the trait-call
// producer, never on "the operand is a reference".
// CHECK-LABEL: fn plain_ref_deref(
// CHECK:         println!("{}", *v0);
emitrust.func @plain_ref_deref(%arg0: !emitrust.ref<i32>) {
  %p = emitrust.deref %arg0 : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
  %v = emitrust.load %p : (!emitrust.lvalue<i32>) -> i32
  emitrust.call_opaque "println!"(%v) {args = ["{}", 0 : index]} : (i32) -> ()
  emitrust.return
}

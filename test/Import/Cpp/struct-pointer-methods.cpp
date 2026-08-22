// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUST

// FR-120 item 1: method calls through struct POINTERS. This file pins the
// IMPORT-LEVEL shape: the pointer is fully ERASED, with ZERO new dialect
// ops. A region-tracked LOCAL receiver (`Counter *p = &c; p->bump(..)`)
// resolves straight to the bound object's own place -- the emitted IR is
// byte-for-byte the direct `c.bump(..)` shape (measured against direct
// twins in the FR-120 spike) -- and a pointer PARAMETER receiver resolves
// through the borrow deref (`emitrust.deref %arg` -> `addr_of` ->
// `call {emitrust.method_call}`), exactly the shape a reference
// parameter already produced. Before FR-120 both receivers died at
// `emitLValue`'s blanket "unsupported use of pointer variable/parameter"
// rejections. The byte-diff oracle is
// test/EndToEnd/cpp-struct-pointer-methods.cpp; what stays rejected is
// pinned in struct-pointer-methods-invalid.cpp.
//
// The RUST prefix pins the E0502 non-recurrence shape: `p->bump(p->get())`
// renders the ARGUMENT as its own `let` with the receiver borrow an
// inline autoref under two-phase borrows (`let v = p.counter_get();
// p.counter_bump(v);`). W2.21's Box path materializes the receiver &mut
// ahead of the arguments and measured E0502; this path cannot collide
// structurally, and the pin holds it there.
//
// This file also pins the FR-120 slice-screen fix: `p->get()` for a
// CONST method through the NON-CONST parameter `Counter *p` stacks a
// NoOp qualification cast over the LValueToRValue read, which the
// single-peel `asPointerParamRef` missed -- so `p` used to classify
// SLICE and every call site died with the scalar-as-slice wording. The
// `via_mut_param` signature pin (`mut_ref`, not a slice) is the fix's
// shape; the FR-100 CONSTPTR pin it must NOT disturb stays in
// test/Import/C/scalar-out-param-forward-invalid.c.

// CHECK: emitrust.struct_def @Counter ["n"] [i32]
struct Counter {
  int n;
  Counter(int v) : n(v) {}
  int get() const { return n; }
  void bump(int d) { n += d; }
};

// The parameter receiver: `p` maps to an ordinary `mut_ref` borrow (NOT a
// slice -- the slice-screen fix), the receiver place is a plain
// `emitrust.deref` of the block argument, and the E0502 shape emits the
// argument's own call chain before handing the value to `bump`.
// CHECK-LABEL: func.func @via_mut_param(
// CHECK-SAME: %arg0: !emitrust.mut_ref<!emitrust.struct<"Counter">>
// CHECK: %[[RECV:.*]] = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<!emitrust.struct<"Counter">>
// CHECK: %[[MREF:.*]] = emitrust.addr_of mut %[[RECV]]
// CHECK: %[[ARECV:.*]] = emitrust.deref %arg0
// CHECK-NEXT: %[[AREF:.*]] = emitrust.addr_of %[[ARECV]]
// CHECK-NEXT: %[[GOT:.*]] = call @Counter_get(%[[AREF]]) {emitrust.method_call}
// CHECK-NEXT: call @Counter_bump(%[[MREF]], %[[GOT]]) {emitrust.method_call}
// RUST-LABEL: fn tu0_via_mut_param(p: &mut Counter)
// RUST-NEXT: let v{{[0-9]+}}: i32 = p.counter_get();
// RUST-NEXT: p.counter_bump(v{{[0-9]+}});
static void via_mut_param(Counter *p) { p->bump(p->get()); }

// The local receiver: the pointer is ERASED. Every call lands directly on
// `c`'s own place -- no deref, no pointer variable, no new op; the
// mutating receiver takes `addr_of mut`, the const receiver `addr_of`.
// CHECK-LABEL: func.func @use_(
// CHECK: %[[C:.*]] = emitrust.variable named "c" : !emitrust.lvalue<!emitrust.struct<"Counter">>
// CHECK: call @Counter_new(
// CHECK: %[[M:.*]] = emitrust.addr_of mut %[[C]]
// CHECK-NEXT: %[[R:.*]] = emitrust.addr_of %[[C]]
// CHECK-NEXT: %[[V:.*]] = call @Counter_get(%[[R]]) {emitrust.method_call}
// CHECK-NEXT: call @Counter_bump(%[[M]], %[[V]]) {emitrust.method_call}
// The (*p).m() spelling: same erasure, same place.
// CHECK: %[[M2:.*]] = emitrust.addr_of mut %[[C]]
// CHECK: call @Counter_bump(%[[M2]],
// CHECK: %[[ARG:.*]] = emitrust.addr_of mut %[[C]]
// CHECK-NEXT: call @via_mut_param(%[[ARG]])
// CHECK-NOT: unsupported
int use(int argc) {
  Counter c(argc);
  Counter *p = &c;
  p->bump(p->get()); // E0502 shape through a LOCAL
  (*p).bump(2);      // explicit-deref spelling
  via_mut_param(&c);
  return p->get() + c.n;
}

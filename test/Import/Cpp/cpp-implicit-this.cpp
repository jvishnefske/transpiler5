// RUN: emitrust-import-c %s | FileCheck %s

// FR-47: an intra-class method call through the IMPLICIT `this` receiver.
//
// W2.2 landed `this` as an rvalue (`emitRValue`'s `CXXThisExpr` case) and as
// the base of a `->` member access, but never as an assignable PLACE. That
// gap was invisible for field access — `value = value + 1` goes through
// `emitMemberBasePlace`, which derefs the receiver itself — and only
// surfaced at a method CALL, because `emitCXXMemberCall` borrows the
// receiver's place with `emitrust.addr_of`. Clang spells the implicit object
// argument of both `m();` and `this->m();` as a bare `CXXThisExpr`, so both
// reached `emitLValue`'s tail and rejected with
// "unsupported assignable expression: CXXThisExpr", blocking every class
// whose methods call each other.
//
// The PIN, and the reason this shape is not new ground: `(*this).m()`
// already worked before FR-47, because `*this` is a `UnaryOperator` that
// reaches `emitDerefLValue`. The fix makes the bare `CXXThisExpr` build the
// IDENTICAL op sequence, so all three spellings are indistinguishable in the
// emitted IR:
//
//   %r = emitrust.deref %self          -> !emitrust.lvalue<struct>
//   %b = emitrust.addr_of [mut] %r     -> !emitrust.[mut_]ref<struct>
//   call @Struct_callee(%b) {emitrust.method_call}
//
// Receiver mutability is NOT a new rule: it stays `emitCXXMemberCall`'s
// pre-existing `is_mut = !method->isConst()`, paired with the receiver type
// `importFunction` already fixed at signature time (`mut_ref` for a mutating
// method or constructor, `ref` for a `const` one). A `const` caller can only
// ever reach a `const` callee, because C++ itself rejects the other
// direction before the importer runs — so the `&self`/`&mut self` decision
// is made once, by the CALLEE, exactly as it was for an explicit receiver.
//
// Also pinned here: FR-47's declare-then-define prepass in
// `importCXXMethods`. A C++ member function body is a complete-class
// context, so a method may call a sibling DECLARED LATER; the single-pass,
// declaration-order import this file's predecessor (methods.cpp) was built
// around could not resolve that and rejected with "call to unimported
// method". `Order` and `Ctor` below are the two shapes that needs — a
// forward call and a constructor calling a method declared after it.

class Bumper {
public:
  // Mutating callee: `mut_ref` receiver, reached from `both` below.
  void bump() { v_ = v_ + 1; }

  // THE MINIMAL REPRO. Two implicit-`this` calls in one body, so the pin
  // also covers a second borrow of the same receiver in the same function
  // (each call materializes its own deref + borrow; no borrow outlives its
  // call, which is what keeps the emitted Rust borrow-checkable).
  void both() {
    bump();
    bump();
  }

  // The same call written with an explicit `this->`. Clang's implicit
  // object argument is a `CXXThisExpr` here too, so this must produce
  // byte-identical IR to `both`'s per-call shape.
  void viaArrow() { this->bump(); }

  // The spelling that ALREADY worked (an explicit `*this` deref), kept
  // adjacent so a future change that diverges the two paths shows up as a
  // diff between these two checks rather than as a silent asymmetry.
  void viaStar() { (*this).bump(); }

  int value() const { return v_; }

private:
  int v_;
};

// A `const` method calling another `const` method: the CALLER's receiver is
// already a plain `ref`, and the borrow must be a plain `addr_of` (NOT
// `addr_of mut`) because the CALLEE is const. This is the case that would
// have needed a second mutability rule had the fix decided mutability at the
// place rather than leaving it to `emitCXXMemberCall`.
class Reader {
public:
  int raw() const { return v_; }
  int scaled() const { return raw() * 2; }
  void set(int x) { v_ = x; }

private:
  int v_;
};

// A NON-const (mutating) caller reaching a `const` callee: the receiver
// argument is `mut_ref`, the borrow is shared. Real C++ allows this
// (a `&mut T` reborrows as `&T` in Rust just as a non-const object binds to
// a const member function in C++), and it is the mixed case the two classes
// above do not cover on their own.
class Mixed {
public:
  int peek() const { return v_; }
  void grow() { v_ = v_ + peek(); }

private:
  int v_;
};

// Sibling declared AFTER its caller, and a constructor calling a method
// declared after it: both resolve only because of the signature prepass.
class Order {
public:
  Order() : v_(0) { reset(); }
  int start() { return later(); }
  int later() { return v_; }
  void reset() { v_ = 7; }

private:
  int v_;
};

int drive(void) {
  Bumper b;
  b.both();
  b.viaArrow();
  b.viaStar();
  Reader r;
  r.set(3);
  Mixed m;
  m.grow();
  Order o;
  return b.value() + r.scaled() + m.peek() + o.start();
}

// CHECK: emitrust.struct_def @Bumper ["v_"] [i32]

// The repro: TWO independent implicit-`this` call sites in one body. Each
// one derefs the receiver argument afresh and borrows it mutably (the callee
// `bump` is non-const), and the borrow feeds the call directly with nothing
// in between — the one-statement borrow shape the FuncToEmitRust method-call
// rewrite requires (it demands the receiver operand be a single-use
// `emitrust.addr_of`).
// CHECK-LABEL: func.func @Bumper_both
// CHECK-SAME: (%[[BSELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"Bumper">>)
// CHECK-SAME: attributes {emitrust.method_of = "Bumper"}
// CHECK: %[[B1:.*]] = emitrust.deref %[[BSELF]] : (!emitrust.mut_ref<!emitrust.struct<"Bumper">>) -> !emitrust.lvalue<!emitrust.struct<"Bumper">>
// CHECK-NEXT: %[[B1REF:.*]] = emitrust.addr_of mut %[[B1]] : (!emitrust.lvalue<!emitrust.struct<"Bumper">>) -> !emitrust.mut_ref<!emitrust.struct<"Bumper">>
// CHECK-NEXT: call @Bumper_bump(%[[B1REF]]) {emitrust.method_call}
// CHECK-NEXT: %[[B2:.*]] = emitrust.deref %[[BSELF]] : (!emitrust.mut_ref<!emitrust.struct<"Bumper">>) -> !emitrust.lvalue<!emitrust.struct<"Bumper">>
// CHECK-NEXT: %[[B2REF:.*]] = emitrust.addr_of mut %[[B2]] : (!emitrust.lvalue<!emitrust.struct<"Bumper">>) -> !emitrust.mut_ref<!emitrust.struct<"Bumper">>
// CHECK-NEXT: call @Bumper_bump(%[[B2REF]]) {emitrust.method_call}

// The explicit `this->` spelling: identical shape, one call.
// CHECK-LABEL: func.func @Bumper_viaArrow
// CHECK-SAME: (%[[ASELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"Bumper">>)
// CHECK: %[[A1:.*]] = emitrust.deref %[[ASELF]] : (!emitrust.mut_ref<!emitrust.struct<"Bumper">>) -> !emitrust.lvalue<!emitrust.struct<"Bumper">>
// CHECK-NEXT: %[[A1REF:.*]] = emitrust.addr_of mut %[[A1]] : (!emitrust.lvalue<!emitrust.struct<"Bumper">>) -> !emitrust.mut_ref<!emitrust.struct<"Bumper">>
// CHECK-NEXT: call @Bumper_bump(%[[A1REF]]) {emitrust.method_call}

// The pre-existing `(*this).` spelling, for shape equality with the two
// above (this is the check that would break first if the new place branch
// ever diverged from `emitDerefLValue`).
// CHECK-LABEL: func.func @Bumper_viaStar
// CHECK-SAME: (%[[SSELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"Bumper">>)
// CHECK: %[[S1:.*]] = emitrust.deref %[[SSELF]] : (!emitrust.mut_ref<!emitrust.struct<"Bumper">>) -> !emitrust.lvalue<!emitrust.struct<"Bumper">>
// CHECK-NEXT: %[[S1REF:.*]] = emitrust.addr_of mut %[[S1]] : (!emitrust.lvalue<!emitrust.struct<"Bumper">>) -> !emitrust.mut_ref<!emitrust.struct<"Bumper">>
// CHECK-NEXT: call @Bumper_bump(%[[S1REF]]) {emitrust.method_call}

// `const` caller -> `const` callee. BOTH the receiver argument and the
// borrow are shared: `!emitrust.ref`, and `emitrust.addr_of` with NO `mut`.
// CHECK: emitrust.struct_def @Reader ["v_"] [i32]
// CHECK-LABEL: func.func @Reader_scaled
// CHECK-SAME: (%[[RSELF:.*]]: !emitrust.ref<!emitrust.struct<"Reader">>) -> i32
// CHECK-SAME: attributes {emitrust.method_of = "Reader"}
// CHECK: %[[R1:.*]] = emitrust.deref %[[RSELF]] : (!emitrust.ref<!emitrust.struct<"Reader">>) -> !emitrust.lvalue<!emitrust.struct<"Reader">>
// CHECK-NEXT: %[[R1REF:.*]] = emitrust.addr_of %[[R1]] : (!emitrust.lvalue<!emitrust.struct<"Reader">>) -> !emitrust.ref<!emitrust.struct<"Reader">>
// CHECK-NEXT: call @Reader_raw(%[[R1REF]]) {emitrust.method_call}

// Mutating caller -> `const` callee: `mut_ref` receiver argument, but a
// SHARED borrow, since the borrow follows the callee's constness.
// CHECK: emitrust.struct_def @Mixed ["v_"] [i32]
// CHECK-LABEL: func.func @Mixed_grow
// CHECK-SAME: (%[[MSELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"Mixed">>)
// CHECK-SAME: attributes {emitrust.method_of = "Mixed"}
// (Anchored on the borrow rather than on the deref: `grow`'s body reads
// `v_` before it calls, so the call's own deref is not the body's first.)
// CHECK: %[[M1REF:.*]] = emitrust.addr_of %{{.*}} : (!emitrust.lvalue<!emitrust.struct<"Mixed">>) -> !emitrust.ref<!emitrust.struct<"Mixed">>
// CHECK-NEXT: call @Mixed_peek(%[[M1REF]]) {emitrust.method_call}

// The declaration-order prepass. Emitted method order still follows
// DECLARATION order (the prepass's stubs are erased and rebuilt in place by
// the definition pass), so the constructor comes first even though it calls
// a method defined two declarations later.
// CHECK: emitrust.struct_def @Order ["v_"] [i32]
// CHECK-LABEL: func.func @Order_new
// CHECK: call @Order_reset(%{{.*}}) {emitrust.method_call}
// CHECK-LABEL: func.func @Order_start
// CHECK: call @Order_later(%{{.*}}) {emitrust.method_call}
// CHECK-LABEL: func.func @Order_later
// CHECK-LABEL: func.func @Order_reset

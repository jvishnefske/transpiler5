// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUST

// W2.18: single non-virtual inheritance lowers the base to an ordinary
// FIRST field named `base`, with every inherited access flattened through
// it. This file pins the IMPORT-LEVEL shape (raw struct_def/func.func, pre
// convert-func-to-emitrust) and the EMITTED RUST shape of the admitted
// subset; the byte-diff oracle for the wave is
// test/EndToEnd/cpp-inheritance.cpp and corpus entry
// test/Cpp17Suite/Inputs/01004.cpp.
//
// W2.0 rejected any base class outright, on the ground that walking
// `fields()` alone would SILENTLY DROP the inherited data. That reasoning
// still binds and this wave does not relax it -- it satisfies it: the base
// is materialised as a real field, so nothing is dropped, and the
// rejection narrows to the shapes that have no such image (see
// inheritance-invalid.cpp).
//
// The three things that carry the wave, none of them a new op:
//
// 1. The synthesized field. `collectRecordFields` PREPENDS `base` before
//    walking `fields()`, matching the C++ object's own layout and putting
//    the name into `appendField`'s duplicate-guard set (so a derived
//    member literally spelled `base` is a located collision, not a silent
//    overwrite).
// 2. Inherited access = one `emitrust.member ["base"]` per hop. clang
//    spells an inherited access as an implicit
//    `<UncheckedDerivedToBase>`/`<DerivedToBase>` cast over the receiver,
//    with a MULTI-ENTRY `CastExpr::path()` for a multi-level chain, so a
//    two-hop projection is `self.base.base` and comes from ONE cast node.
// 3. The base initializer. `Derived(...) : Base(a)` is a
//    `CXXCtorInitializer` with `isBaseInitializer()`, routed to the
//    place-based `emitCXXConstructInit` against `self.base` -- the same
//    `addr_of mut` + `emitrust.method_call` shape a nested member's
//    constructor call already emits.

extern "C" int printf(const char *, ...);

// CHECK: emitrust.struct_def @Base ["x"] [i32]
struct Base {
  int x;
  Base(int v) : x(v) {}
  int get() const { return x; }
  void bump(int d) { x += d; }
};

// The base is the FIRST field, ahead of the derived class's own members,
// and it is a real struct-typed field rather than an opaque blob.
// CHECK: emitrust.struct_def @Derived ["base", "y"] [!emitrust.struct<"Base">, i32]
struct Derived : Base {
  int y;

  // The base initializer: `member ["base"]`, `addr_of mut`, then the
  // base's own imported constructor. NOT an `emitrust.assign` of a value
  // (the value-position constructor path rejects a class-typed
  // initializer, by design).
  // CHECK-LABEL: func.func @Derived_ctor(
  // CHECK: %[[SELF:.*]] = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Derived">>) -> !emitrust.lvalue<!emitrust.struct<"Derived">>
  // CHECK-NEXT: %[[BASE:.*]] = emitrust.member %[[SELF]]["base"] : (!emitrust.lvalue<!emitrust.struct<"Derived">>) -> !emitrust.lvalue<!emitrust.struct<"Base">>
  // CHECK-NEXT: %[[REF:.*]] = emitrust.addr_of mut %[[BASE]] : (!emitrust.lvalue<!emitrust.struct<"Base">>) -> !emitrust.mut_ref<!emitrust.struct<"Base">>
  // CHECK: call @Base_ctor(%[[REF]], {{.*}}) {emitrust.method_call}
  // CHECK: emitrust.member {{.*}}["y"]
  Derived(int a, int b) : Base(a), y(b) {}

  // The IMPLICIT inherited call (`get()` with no `this->`): the receiver
  // is `this` projected through `base`, and the borrow is SHARED because
  // `Base::get` is const.
  // CHECK-LABEL: func.func @Derived_sum(
  // CHECK: %[[S:.*]] = emitrust.deref %arg0
  // CHECK-NEXT: %[[B:.*]] = emitrust.member %[[S]]["base"] : (!emitrust.lvalue<!emitrust.struct<"Derived">>) -> !emitrust.lvalue<!emitrust.struct<"Base">>
  // CHECK-NEXT: %[[R:.*]] = emitrust.addr_of %[[B]] : (!emitrust.lvalue<!emitrust.struct<"Base">>) -> !emitrust.ref<!emitrust.struct<"Base">>
  // CHECK-NEXT: call @Base_get(%[[R]]) {emitrust.method_call}
  int sum() const { return get() + y; }

  // An inherited FIELD read: two chained members, no call at all.
  // CHECK-LABEL: func.func @Derived_scaled(
  // CHECK: %[[S2:.*]] = emitrust.deref %arg0
  // CHECK-NEXT: %[[B2:.*]] = emitrust.member %[[S2]]["base"] : (!emitrust.lvalue<!emitrust.struct<"Derived">>) -> !emitrust.lvalue<!emitrust.struct<"Base">>
  // CHECK-NEXT: emitrust.member %[[B2]]["x"] : (!emitrust.lvalue<!emitrust.struct<"Base">>) -> !emitrust.lvalue<i32>
  int scaled() const { return x * 3 + y; }

  // An inherited field WRITE next to an inherited MUTATING call: the
  // assign lands on `base.x`, and the call borrows `base` mutably.
  // CHECK-LABEL: func.func @Derived_grow(
  // CHECK: %[[B3:.*]] = emitrust.member {{.*}}["base"]
  // CHECK-NEXT: %[[X3:.*]] = emitrust.member %[[B3]]["x"]
  // CHECK: emitrust.assign %[[X3]] =
  // CHECK: %[[B4:.*]] = emitrust.member {{.*}}["base"]
  // CHECK-NEXT: %[[R4:.*]] = emitrust.addr_of mut %[[B4]] : (!emitrust.lvalue<!emitrust.struct<"Base">>) -> !emitrust.mut_ref<!emitrust.struct<"Base">>
  // CHECK: call @Base_bump(%[[R4]], {{.*}}) {emitrust.method_call}
  void grow(int d) { x += d; bump(d); y += d; }

  // The EXPLICIT `this->` spellings import identically to the implicit
  // ones: clang differs only in an `implicit` marker on the CXXThisExpr.
  // CHECK-LABEL: func.func @Derived_viaThis(
  // CHECK: emitrust.member {{.*}}["base"]
  // CHECK: call @Base_get({{.*}}) {emitrust.method_call}
  // CHECK: %[[B5:.*]] = emitrust.member {{.*}}["base"]
  // CHECK-NEXT: emitrust.member %[[B5]]["x"]
  int viaThis() const { return this->get() + this->x; }

  // The QUALIFIED spelling adds a `CK_NoOp` cast above the derived-to-base
  // one in the AST; the peel is transparent to it, so the projection is
  // the same single hop and no second `base` appears.
  // CHECK-LABEL: func.func @Derived_viaQualified(
  // CHECK: %[[S6:.*]] = emitrust.deref %arg0
  // CHECK-NEXT: %[[B6:.*]] = emitrust.member %[[S6]]["base"] : (!emitrust.lvalue<!emitrust.struct<"Derived">>) -> !emitrust.lvalue<!emitrust.struct<"Base">>
  // CHECK-NEXT: %[[R6:.*]] = emitrust.addr_of %[[B6]]
  // CHECK-NEXT: call @Base_get(%[[R6]]) {emitrust.method_call}
  int viaQualified() const { return Base::get(); }
};

// A two-level chain. `Third`'s base field holds a `Derived`, whose base
// field holds a `Base`, so `Base::get()` from `Third` is a DOUBLE
// projection -- and clang delivers it as ONE cast node with a two-entry
// path, derived-most first.
// CHECK: emitrust.struct_def @Third ["base", "z"] [!emitrust.struct<"Derived">, i32]
struct Third : Derived {
  int z;

  // A base initializer whose base is itself a derived class: the call is
  // to `Derived_ctor`, not to `Base_ctor`, so the chain constructs one level
  // at a time exactly like C++ does.
  // CHECK-LABEL: func.func @Third_ctor(
  // CHECK: %[[TB:.*]] = emitrust.member {{.*}}["base"] : (!emitrust.lvalue<!emitrust.struct<"Third">>) -> !emitrust.lvalue<!emitrust.struct<"Derived">>
  // CHECK-NEXT: %[[TR:.*]] = emitrust.addr_of mut %[[TB]]
  // CHECK: call @Derived_ctor(%[[TR]], {{.*}}, {{.*}}) {emitrust.method_call}
  Third(int a, int b, int c) : Derived(a, b), z(c) {}

  // CHECK-LABEL: func.func @Third_total(
  // CHECK: %[[H1:.*]] = emitrust.member {{.*}}["base"] : (!emitrust.lvalue<!emitrust.struct<"Third">>) -> !emitrust.lvalue<!emitrust.struct<"Derived">>
  // CHECK-NEXT: %[[H2:.*]] = emitrust.member %[[H1]]["base"] : (!emitrust.lvalue<!emitrust.struct<"Derived">>) -> !emitrust.lvalue<!emitrust.struct<"Base">>
  // CHECK-NEXT: %[[H3:.*]] = emitrust.addr_of %[[H2]]
  // CHECK-NEXT: call @Base_get(%[[H3]]) {emitrust.method_call}
  int total() const { return get() + x + y + z; }
};

// An EMPTY base carries no inherited data, so it contributes NO field at
// all: a `[u8; 1]` placeholder would be pure invention (the C++ object
// applies the empty-base optimization and adds no storage). The derived
// struct is exactly its own members.
// CHECK: emitrust.struct_def @Tagged ["v"] [i32]
struct Marker {};
struct Tagged : Marker {
  int v;
};

int main() {
  Derived d(3, 4);
  Third t(10, 20, 30);
  Tagged g;
  g.v = 1;
  printf("%d %d %d %d\n", d.sum(), d.viaThis(), t.total(), g.v);
  return 0;
}

// The emitted Rust: `base` is a plain field, and every inherited access is
// an ordinary nested place expression. No trait, no vtable, no `Deref`.
// RUST: struct Derived {
// RUST-NEXT: base: Base,
// RUST-NEXT: y: i32,
// RUST: struct Third {
// RUST-NEXT: base: Derived,
// RUST-NEXT: z: i32,
// RUST: struct Tagged {
// RUST-NEXT: v: i32,
// RUST: impl Derived {
// RUST: fn ctor(&mut self, a: i32, b: i32) {
// RUST-NEXT: self.base.ctor(a);
// RUST-NEXT: self.y = b;
// RUST: fn sum(&self) -> i32 {
// RUST-NEXT: let v0: i32 = self.base.get();
// RUST-NEXT: v0 + self.y
// RUST: fn scaled(&self) -> i32 {
// RUST-NEXT: self.base.x * 3i32 + self.y
// RUST: fn grow(&mut self, d: i32) {
// RUST-NEXT: self.base.x += d;
// RUST-NEXT: self.base.bump(d);
// RUST-NEXT: self.y += d;
// RUST: fn via_this(&self) -> i32 {
// RUST-NEXT: let v0: i32 = self.base.get();
// RUST-NEXT: v0 + self.base.x
// RUST: fn via_qualified(&self) -> i32 {
// RUST-NEXT: self.base.get()
// RUST: impl Third {
// RUST: fn ctor(&mut self, a: i32, b: i32, c: i32) {
// RUST-NEXT: self.base.ctor(a, b);
// RUST-NEXT: self.z = c;
// RUST: fn total(&self) -> i32 {
// RUST-NEXT: let v0: i32 = self.base.base.get();
// RUST-NEXT: v0 + self.base.base.x + self.base.y + self.z

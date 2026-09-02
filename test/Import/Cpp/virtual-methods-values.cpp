// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUST

// W2.19a: virtual methods on VALUES only -- static dispatch. This file
// pins the IMPORT-LEVEL shape: a class with virtual methods (override,
// final, multi-level) imports as a PLAIN struct_def, its virtual methods
// as ordinary imported methods, and every call on a VALUE receiver as the
// ordinary `{emitrust.method_call}` func call -- ZERO new ops, no trait,
// no dyn, no vtable. That is correct, not approximate: for a value the
// C++ static type IS the dynamic type, so clang's own
// `call->getMethodDecl()` already names the exact override C++ would
// dispatch to (the W2.19 spike byte-diffed this emission against the
// devirtualized twin -- identical). The resolution rule pinned here:
// a call to a method the receiver's class OVERRIDES binds that override
// directly, with NO base hop (`d.noise()` -> `Dog_noise`); a call to a
// virtual method it merely INHERITS peels the DerivedToBase hops and
// replays them as `member ["base"]` projections onto the declaring
// class's own method (`d.weight()` -> `d.base` -> `Animal_weight`),
// exactly the W2.18 flattening non-virtual methods already use.
//
// Every DYNAMIC channel -- any pointer receiver (including implicit
// `this`), polymorphic upcasts, sizeof/alignof, base references,
// new/delete -- stays a located rejection, pinned in
// virtual-methods-values-invalid.cpp. The byte-diff oracle is
// test/EndToEnd/cpp-virtual-methods-values.cpp and corpus entry
// test/Cpp17Suite/Inputs/01009.cpp.

extern "C" int printf(const char *, ...);

// The vptr the native layout carries is deliberately NOT materialized:
// the struct_def holds the data members only. The layout divergence is
// refused where it could be observed -- `emitSizeofAlignof` screens
// polymorphic operands (W2.26).
// CHECK: emitrust.struct_def @Animal ["legs"] [i32]
struct Animal {
  int legs;
  Animal(int l) : legs(l) {}
  virtual int noise() { return 100 + legs; }
  virtual int weight() { return 10 + legs; }
  int tag() { return legs * 7; }
};

// CHECK: emitrust.struct_def @Dog ["base", "bark"] [!emitrust.struct<"Animal">, i32]
struct Dog : Animal {
  int bark;
  Dog(int l, int b) : Animal(l), bark(b) {}
  int noise() override { return 200 + bark + legs; }
};

// CHECK: emitrust.struct_def @Puppy ["base"] [!emitrust.struct<"Dog">]
struct Puppy : Dog {
  Puppy(int l, int b) : Dog(l, b) {}
  int noise() override final { return 300 + bark; }
  int weight() override { return 3 + legs; }
};

// An ABSTRACT base is admissible as a BASE only: the class itself can
// never be a value (clang rejects instantiation upstream), but the base
// SUBOBJECT of a concrete derived value is constructible. The pure
// method has no body, so it rides the FR-47 omission channel: the
// signature prepass registers an external stub, the UNCALLED stub is
// erased from the finalized module (the `@Pure_f`-free pin below), and
// any use fails LOUDLY at project finalization (`referenced but not
// defined in any translation unit` -- pinned in
// virtual-methods-values-invalid.cpp).
// CHECK: emitrust.struct_def @Pure ["x"] [i32]
struct Pure {
  int x;
  virtual int f() = 0;
};
// CHECK: emitrust.struct_def @Impl ["base"] [!emitrust.struct<"Pure">]
struct Impl : Pure {
  int f() override { return 5 + x; }
};

// CHECK-LABEL: func.func @calls(
// CHECK: %[[D:.*]] = emitrust.variable named "d"
// CHECK: call @Dog_ctor(
// CHECK: call @Puppy_ctor(
// The override CASE: `d.noise()` binds Dog's own override, directly on
// `d`'s place -- no hop, no dispatch machinery.
// CHECK: %[[DREF:.*]] = emitrust.addr_of mut %[[D]] : (!emitrust.lvalue<!emitrust.struct<"Dog">>) -> !emitrust.mut_ref<!emitrust.struct<"Dog">>
// CHECK-NEXT: call @Dog_noise(%[[DREF]]) {emitrust.method_call}
// The inherited CASE: `d.weight()` -- Dog has no override, so the
// DerivedToBase hop replays as `member ["base"]` and the call binds the
// DECLARING class's method.
// CHECK-NEXT: %[[DB:.*]] = emitrust.member %[[D]]["base"] : (!emitrust.lvalue<!emitrust.struct<"Dog">>) -> !emitrust.lvalue<!emitrust.struct<"Animal">>
// CHECK-NEXT: %[[DBREF:.*]] = emitrust.addr_of mut %[[DB]]
// CHECK-NEXT: call @Animal_weight(%[[DBREF]]) {emitrust.method_call}
// The `final` override and the deeper override both bind Puppy's own
// bodies, again hop-free.
// CHECK: call @Puppy_noise({{.*}}) {emitrust.method_call}
// CHECK: call @Puppy_weight({{.*}}) {emitrust.method_call}
// A NON-virtual method inherited across TWO hops keeps the W2.18
// two-projection flattening, unchanged by the wave.
// CHECK: %[[PB:.*]] = emitrust.member %[[P:.*]]["base"] : (!emitrust.lvalue<!emitrust.struct<"Puppy">>) -> !emitrust.lvalue<!emitrust.struct<"Dog">>
// CHECK-NEXT: %[[PBB:.*]] = emitrust.member %[[PB]]["base"] : (!emitrust.lvalue<!emitrust.struct<"Dog">>) -> !emitrust.lvalue<!emitrust.struct<"Animal">>
// CHECK-NEXT: %[[PBBREF:.*]] = emitrust.addr_of mut %[[PBB]]
// CHECK-NEXT: call @Animal_tag(%[[PBBREF]]) {emitrust.method_call}
int calls(int n) {
  Dog d(n, n + 1);
  Puppy p(n + 2, n + 3);
  int r = d.noise() + d.weight();
  r += p.noise() + p.weight();
  r += p.tag();
  return r;
}

// The concrete derived value over the abstract base: `i.f()` binds the
// override; the pure virtual method itself, uncalled and body-less, is
// erased from the finalized module entirely (the FR-47 stub channel).
// CHECK-LABEL: func.func @pure_value(
// CHECK: call @Impl_f({{.*}}) {emitrust.method_call}
// CHECK-NOT: @Pure_f
int pure_value(int n) {
  Impl i;
  i.x = n;
  return i.f();
}

// The emitted Rust is ordinary inherent-method calls on values -- the
// same surface non-virtual methods use. Nothing dynamic exists to emit.
// RUST-LABEL: fn calls(
// RUST: d.noise()
// RUST: d.base.weight()
// RUST: p.noise()
// RUST: p.weight()
// RUST: p.base.base.tag()
// RUST-NOT: dyn
// RUST-NOT: trait
// RUST-NOT: Box
int main() {
  printf("%d %d\n", calls(3), pure_value(4));
  return 0;
}

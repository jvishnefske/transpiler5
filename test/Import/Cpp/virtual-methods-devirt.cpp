// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUST

// W2.19b: DEVIRTUALIZED single-object base pointer. This file pins the
// import-level shape for a VIRTUAL call through a pointer whose region
// binds EXACTLY ONE local object (the FR-120 single-object region fact):
// the call resolves at COMPILE TIME against that object's most-derived
// type -- devirtualization, not dispatch -- with ZERO new ops, no trait,
// no dyn, no vtable, and the pointer local fully ERASED from emission.
// That is exact C++ semantics, not an approximation: a pointer that can
// only ever designate one object has that object's dynamic type, so the
// final overrider is statically known (the W2.19 spike byte-diffed this
// emission against clang++ -- identical, including 00901).
//
// The resolution rules pinned here (legD4's two halves plus the flips):
// * a VIRTUAL call through the base pointer binds the object's DYNAMIC
//   type's override, directly on the derived place, NO hop
//   (`p->calc()` -> `Der_calc` on `d`);
// * a virtual method the derived class merely INHERITS devirtualizes to
//   the declaring class's own body THROUGH the `member ["base"]` hop
//   chain (`p->fixed()` -> `d.base` -> `Base_fixed`);
// * a NON-virtual call through the same pointer keeps the pointer's
//   STATIC type's binding (`p->tag()` -> `Base_tag` on `d.base`, even
//   though `Der` declares its own `tag`) -- C++'s static-bind rule,
//   unchanged by this wave;
// * three pins moved FORWARD from located rejections (the pin moves
//   forward, never loosens): virtual-methods-values-invalid.cpp's
//   PTRLOCAL/PTRDEREF arms (same-type local pointer: the pointer's one
//   object IS the type), its VUPCASTM arm and inheritance-invalid.cpp's
//   VIRTMETHOD arm (devirt through the polymorphic upcast / same-type
//   derived pointer), and inheritance-drop-invalid.cpp's VUPCAST arm (a
//   NON-virtual call through a sole-virtual-dtor base pointer is now a
//   working static bind).
//
// Everything the single-object fact does not cover stays a located
// rejection, pinned in virtual-methods-values-invalid.cpp: multi-object
// pointers (the soundness floor -- devirt could pick wrong), pointer
// parameters (open caller set), implicit `this`, ctor bodies, global
// objects, nullable regions, qualified calls, and the W2.21 Box payload.
// The byte-diff oracle is test/EndToEnd/cpp-devirt-base-pointer.cpp and
// corpus entry test/Cpp17Suite/Inputs/00901.cpp.

extern "C" int printf(const char *, ...);

// CHECK: emitrust.struct_def @Base ["seed"] [i32]
struct Base {
  int seed;
  virtual int calc() { return seed * 2; }
  virtual int fixed() { return 40 + seed; }
  int tag() { return 100 + seed; }
};

// CHECK: emitrust.struct_def @Der ["base", "extra"] [!emitrust.struct<"Base">, i32]
struct Der : Base {
  int extra;
  int calc() override { return seed + extra; }
  int tag() { return 200 + seed; }
};

// The legD4 shape: one base pointer, all three resolution rules. The
// pointer `p` is compile-time erased -- no cell, no binding, pinned by
// the absence of any variable named "p" below.
// CHECK-LABEL: func.func @through_base(
// CHECK-NOT: named "p"
// Virtual override: devirtualized to Der's own body, on `d`'s own place,
// NO hop projection (FR-120's carry-forward: the receiver reconciles
// toward the devirt TARGET's class, not the pointer's static pointee).
// CHECK: %[[DREF:.*]] = emitrust.addr_of mut %[[D:.*]] : (!emitrust.lvalue<!emitrust.struct<"Der">>) -> !emitrust.mut_ref<!emitrust.struct<"Der">>
// CHECK-NEXT: call @Der_calc(%[[DREF]]) {emitrust.method_call}
// Inherited virtual: devirtualized to the declaring class's body through
// the recomputed `member ["base"]` hop chain.
// CHECK: %[[DB:.*]] = emitrust.member %[[D]]["base"] : (!emitrust.lvalue<!emitrust.struct<"Der">>) -> !emitrust.lvalue<!emitrust.struct<"Base">>
// CHECK-NEXT: %[[DBREF:.*]] = emitrust.addr_of mut %[[DB]]
// CHECK-NEXT: call @Base_fixed(%[[DBREF]]) {emitrust.method_call}
// Non-virtual through the SAME pointer: static bind to the pointer's
// static pointee (Base::tag, not Der::tag) through the same hop chain.
// CHECK: %[[DB2:.*]] = emitrust.member %[[D]]["base"]
// CHECK-NEXT: %[[DB2REF:.*]] = emitrust.addr_of mut %[[DB2]]
// CHECK-NEXT: call @Base_tag(%[[DB2REF]]) {emitrust.method_call}
// CHECK-NOT: @Der_tag(
int through_base(int n) {
  Der d;
  d.seed = n;
  d.extra = n * 3;
  Base *p = &d;
  return p->calc() + p->fixed() + p->tag();
}

// The W2.19a carve-out flip: a SAME-TYPE local pointer virtual call
// (PTRLOCAL) and its `(*q).f()` spelling (PTRDEREF) -- the pointer's one
// object IS the type, so devirt binds the object's own override
// directly.
// CHECK: emitrust.struct_def @S ["k"] [i32]
struct S {
  int k;
  virtual int f() { return 1 + k; }
};
// CHECK-LABEL: func.func @same_type(
// CHECK-NOT: named "q"
// CHECK: call @S_f({{.*}}) {emitrust.method_call}
// CHECK: call @S_f({{.*}}) {emitrust.method_call}
int same_type(int n) {
  S s;
  s.k = n;
  S *q = &s;
  return q->f() + (*q).f();
}

// The VIRTMETHOD flip: an inherited virtual through a same-type DERIVED
// pointer -- no upcast at the binding, devirt recomputes the hop to the
// declaring class.
// CHECK: emitrust.struct_def @A2 ["a"] [i32]
struct A2 {
  int a;
  virtual int geta() const { return a; }
};
// CHECK: emitrust.struct_def @D2 ["base", "c"] [!emitrust.struct<"A2">, i32]
struct D2 : A2 {
  int c;
};
// CHECK-LABEL: func.func @derived_ptr(
// The `d2.a = n` / `d2.c = n + 1` field writes project their own hops;
// the `["c"]` anchor below skips past them to the CALL's projection.
// CHECK: emitrust.member %{{.*}}["c"]
// CHECK: %[[E:.*]] = emitrust.member %{{.*}}["base"] : (!emitrust.lvalue<!emitrust.struct<"D2">>) -> !emitrust.lvalue<!emitrust.struct<"A2">>
// CHECK-NEXT: %[[EREF:.*]] = emitrust.addr_of %[[E]]
// CHECK-NEXT: call @A2_geta(%[[EREF]]) {emitrust.method_call}
int derived_ptr(int n) {
  D2 d2;
  d2.a = n;
  d2.c = n + 1;
  D2 *pd = &d2;
  return pd->geta() + d2.c;
}

// The VUPCAST flip (inheritance-drop-invalid.cpp): a NON-virtual call
// through a sole-virtual-dtor base pointer. The polymorphic chain now
// binds (the peel's isPolymorphic refusal is relaxed under the
// single-object fact) and the call is an ordinary static bind through
// the hop; the virtual DESTRUCTOR stays exact for free -- the pointer
// does not own, so the drop runs at the VALUE's scope exit (`impl Drop
// for V` below).
struct V {
  int id;
  V(int i) : id(i) {}
  int get() const { return id; }
  virtual ~V() { printf("~V %d\n", id); }
};
struct W : V {
  int w;
  W(int i) : V(i), w(i + 1) {}
};
// CHECK-LABEL: func.func @dtor_chain(
// CHECK: %[[WB:.*]] = emitrust.member %{{.*}}["base"] : (!emitrust.lvalue<!emitrust.struct<"W">>) -> !emitrust.lvalue<!emitrust.struct<"V">>
// CHECK-NEXT: %[[WBREF:.*]] = emitrust.addr_of %[[WB]]
// CHECK-NEXT: call @V_get(%[[WBREF]]) {emitrust.method_call}
int dtor_chain(int n) {
  W obj(n);
  V *pv = &obj;
  return pv->get();
}

// The emitted Rust is ordinary inherent-method calls on the bound
// object -- the same surface value calls use. Nothing dynamic exists,
// and no pointer survives to emission.
// RUST-LABEL: fn through_base(
// RUST: d.der_calc()
// RUST: d.base.base_fixed()
// RUST: d.base.base_tag()
// RUST-LABEL: fn same_type(
// RUST: s.s_f()
// RUST-LABEL: fn derived_ptr(
// RUST: d2.base.a2_geta()
// RUST-NOT: dyn
// RUST-NOT: trait
// RUST-NOT: Box
int main() {
  printf("%d %d %d %d\n", through_base(3), same_type(4), derived_ptr(5),
         dtor_chain(6));
  return 0;
}

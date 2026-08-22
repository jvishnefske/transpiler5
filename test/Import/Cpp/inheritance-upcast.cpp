// RUN: emitrust-import-c %s | FileCheck %s

// FR-120 item 3: UPCAST pointer bindings, import-level shape. Two W2.18
// frontier pins moved FORWARD into this file (the pin moves forward,
// never loosens):
// * inheritance-invalid.cpp's UPPTR arm (`A *p = &d; p->get()` died at
//   the planner's "pointer assigned a non-address value");
// * inheritance-invalid.cpp's DERIVEDPTR arm (`p->a` for an inherited
//   `a` died at "inherited member through a pointer to a derived
//   class" -- that wording survives only for non-pointer arrow bases).
//
// The design that flips them, with ZERO new dialect ops and NO third
// binding kind: `peelPointerCast` peels CK_DerivedToBase gated on
// `uniquePublicSingleBaseChain` (single, public, non-virtual,
// NON-POLYMORPHIC bases -- exactly W2.18's admission guard), the region
// binds the WHOLE derived object as an ordinary null-member binding, and
// every struct-place consumer re-derives the hop path via
// `reconcileUpcastPlace` into the existing `projectBaseHops`. The
// reconcile is the SOUNDNESS LINE: `emitrust.member` has no
// field-existence check, so a skipped reconcile is a rustc E0609 -- or,
// under a shadowed field, a silently wrong subobject; the byte-diff
// oracle test/EndToEnd/cpp-upcast-pointer.cpp carries that leg. The
// recomputable-hops argument RESTS on the single-base guard: if multiple
// inheritance is ever admitted, the path stops being unique and a
// stored-path binding kind becomes necessary. What stays rejected
// (multi-object regions, joins, globals, polymorphic chains, base
// params/members/arrays) is pinned in inheritance-invalid.cpp,
// inheritance-drop-invalid.cpp and struct-pointer-methods-invalid.cpp.

// CHECK: emitrust.struct_def @A ["a"] [i32]
struct A {
  int a;
  int geta() const { return a; }
  void adda(int d) { a += d; }
};
// CHECK: emitrust.struct_def @B ["base", "b"] [!emitrust.struct<"A">, i32]
struct B : A { int b; };
// CHECK: emitrust.struct_def @C ["base", "c"] [!emitrust.struct<"B">, i32]
struct C : B { int c; };

// Name hiding + a SHADOWED data member: `Base *` accesses bind Base's
// members (one `base` hop), direct accesses bind Derived's (no hop).
struct Base {
  int x;
  int tag() const { return 1; }
};
struct Derived : Base {
  int x;
  int tag() const { return 2; }
};

// The DERIVEDPTR shape, moved forward: a DERIVED-typed pointer (`B *pb`,
// no upcast at the binding) reaching an INHERITED member and method --
// the decomposed-arrow branch resolves `pb` to `d`'s own place (exact
// type, reconcile is the identity) and the member/receiver paths project
// the explicit hops.
// CHECK-LABEL: func.func @through_derived(
// CHECK: %[[DD:.*]] = emitrust.variable named "d" : !emitrust.lvalue<!emitrust.struct<"B">>
// CHECK: %[[DH:.*]] = emitrust.member %[[DD]]["base"] : (!emitrust.lvalue<!emitrust.struct<"B">>) -> !emitrust.lvalue<!emitrust.struct<"A">>
// CHECK-NEXT: %[[DM:.*]] = emitrust.addr_of mut %[[DH]]
// CHECK: call @A_adda(%[[DM]], {{.*}}) {emitrust.method_call}
// CHECK: %[[GH:.*]] = emitrust.member %[[DD]]["base"]
// CHECK-NEXT: %[[GA:.*]] = emitrust.member %[[GH]]["a"] : (!emitrust.lvalue<!emitrust.struct<"A">>) -> !emitrust.lvalue<i32>
// CHECK-NEXT: emitrust.load %[[GA]]
int through_derived(int n) {
  B d;
  d.b = n;
  B *pb = &d;
  pb->adda(2);
  return pb->a + pb->b;
}

// CHECK-LABEL: func.func @use_(
int use(int n) {
  // The erased UPPTR shape: the pointer VANISHES; every use is one
  // `member ["base"]` per hop off `d`'s own place.
  // CHECK: %[[D:.*]] = emitrust.variable named "d" : !emitrust.lvalue<!emitrust.struct<"B">>
  B d;
  d.b = n; // touches only the derived member: no hop before the pin below
  A *p = &d;
  // Mutating base method through the pointer: hop, then `addr_of mut`.
  // CHECK: %[[H1:.*]] = emitrust.member %[[D]]["base"] : (!emitrust.lvalue<!emitrust.struct<"B">>) -> !emitrust.lvalue<!emitrust.struct<"A">>
  // CHECK-NEXT: %[[MR:.*]] = emitrust.addr_of mut %[[H1]]
  // CHECK: call @A_adda(%[[MR]], {{.*}}) {emitrust.method_call}
  p->adda(2);
  // The DERIVEDPTR shape, moved forward: an inherited FIELD write through
  // the pointer -- hop, member, then an ordinary assign.
  // CHECK: %[[H2:.*]] = emitrust.member %[[D]]["base"]
  // CHECK-NEXT: %[[FA:.*]] = emitrust.member %[[H2]]["a"] : (!emitrust.lvalue<!emitrust.struct<"A">>) -> !emitrust.lvalue<i32>
  // CHECK: emitrust.assign %[[FA]] =
  p->a += 3;
  C e;
  e.c = n; // derived-own member only
  // Two-level chain: ONE binding, TWO recomputed hops (`base` twice).
  // CHECK: %[[E:.*]] = emitrust.variable named "e" : !emitrust.lvalue<!emitrust.struct<"C">>
  A *q = &e;
  // CHECK: %[[E1:.*]] = emitrust.member %[[E]]["base"] : (!emitrust.lvalue<!emitrust.struct<"C">>) -> !emitrust.lvalue<!emitrust.struct<"B">>
  // CHECK-NEXT: %[[E2:.*]] = emitrust.member %[[E1]]["base"] : (!emitrust.lvalue<!emitrust.struct<"B">>) -> !emitrust.lvalue<!emitrust.struct<"A">>
  // CHECK-NEXT: %[[E3:.*]] = emitrust.member %[[E2]]["a"]
  // CHECK-NEXT: emitrust.load %[[E3]]
  int t = q->a;
  Derived h;
  h.x = n; // the DERIVED `x`: member straight off `h`, no hop
  Base *r = &h;
  // Name hiding binds STATICALLY: the pointer's viewed type picks
  // @Base_tag on the hopped subplace; the direct object call picks
  // @Derived_tag on the derived place, no hop.
  // CHECK: %[[HD:.*]] = emitrust.variable named "h" : !emitrust.lvalue<!emitrust.struct<"Derived">>
  // CHECK: %[[HB:.*]] = emitrust.member %[[HD]]["base"] : (!emitrust.lvalue<!emitrust.struct<"Derived">>) -> !emitrust.lvalue<!emitrust.struct<"Base">>
  // CHECK-NEXT: %[[HR:.*]] = emitrust.addr_of %[[HB]]
  // CHECK-NEXT: call @Base_tag(%[[HR]]) {emitrust.method_call}
  // CHECK: %[[DR:.*]] = emitrust.addr_of %[[HD]] : (!emitrust.lvalue<!emitrust.struct<"Derived">>) -> !emitrust.ref<!emitrust.struct<"Derived">>
  // CHECK-NEXT: call @Derived_tag(%[[DR]]) {emitrust.method_call}
  int u = r->tag() + h.tag();
  // The shadowed field: `r->x` is the BASE subobject (hop then member),
  // `h.x` the derived one (member straight off the derived place).
  // CHECK: %[[SB:.*]] = emitrust.member %[[HD]]["base"]
  // CHECK-NEXT: %[[SX:.*]] = emitrust.member %[[SB]]["x"] : (!emitrust.lvalue<!emitrust.struct<"Base">>) -> !emitrust.lvalue<i32>
  // CHECK-NEXT: emitrust.load %[[SX]]
  // CHECK: %[[DX:.*]] = emitrust.member %[[HD]]["x"] : (!emitrust.lvalue<!emitrust.struct<"Derived">>) -> !emitrust.lvalue<i32>
  // CHECK-NEXT: emitrust.load %[[DX]]
  int v = r->x + h.x;
  return t + u + v + p->geta();
}

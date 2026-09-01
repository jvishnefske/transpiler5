// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=NOATTR
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUST
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUSTPOD

// FR-124: the derive(Copy) decision is a FIELD-COPY FIXPOINT, not a
// per-struct attribute check. This file pins the defect channel and its
// fix; the byte-diff oracle is test/EndToEnd/cpp-derive-copy-fixpoint.cpp
// and the dialect-level shape is test/Target/Rust/derive-copy-fixpoint.mlir.
//
// The channel (measured, see design.md FR-124): `virtual ~B() = default`
// is user-DECLARED, so B gets `emitrust.has_drop` and loses Copy -- but
// the inherited-destructor walk deliberately counts only user-PROVIDED
// dtors (the gcc-15 `__pair_base` filter), so a derived class over that B
// carries NO attribute at all. Both predicates are CORRECT about dropping
// (D destroys nothing); before FR-124 the emitter's Copy decision never
// looked at FIELD Copy-ness, derived Copy over the non-Copy `base: B`
// field, and the crate died at rustc E0204 -- exit-0 unbuildable, 60
// times across the corpus. The invariants pinned here:
//
// 1. THE ATTRIBUTES STAY TRUTHFUL: M, T and DC carry neither has_drop nor
//    has_copy_ctor (the NOATTR prefix). FR-124 is emission-side only --
//    propagating has_drop to the derived class would be semantically
//    false and drag the seven use-site gates along with it.
// 2. THE FIXPOINT REACHES DEPTH 3: B (has_drop) -> M -> T all lose Copy
//    through the `base` field chain, while the POD sibling keeps the
//    full `Clone, Copy, Default` derive (the negative pin: nothing the
//    closure does may touch a struct with no non-Copy field anywhere
//    under it).
// 3. W2.23 COMPOSITION: the has_copy_ctor trigger lives in the same seed
//    lambda, so DC over the user-copy-ctor base C loses Copy too -- the
//    next corpus sweep's E0204 refiling, pre-empted.
// 4. By-value use of a newly non-Copy struct is NOT admitted here; that
//    stays a located import rejection, pinned in
//    derive-copy-fixpoint-invalid.cpp.

extern "C" int printf(const char *, ...);

// CHECK-DAG: emitrust.struct_def @B ["tag"] [i32] {{.*}}emitrust.has_drop}
struct B {
  int tag;
  virtual ~B() = default;
};

// Merely inheriting, dtor-less: no attribute of any kind. The emitter's
// fixpoint, not an importer marker, is what strips Copy.
// CHECK-DAG: emitrust.struct_def @M ["base", "mid"] [!emitrust.struct<"B">, i32]
// NOATTR-NOT: emitrust.struct_def @M {{.*}}has_drop
// NOATTR-NOT: emitrust.struct_def @M {{.*}}has_copy_ctor
struct M : B {
  int mid;
};

// CHECK-DAG: emitrust.struct_def @T ["base", "top"] [!emitrust.struct<"M">, f64]
// NOATTR-NOT: emitrust.struct_def @T {{.*}}has_drop
// NOATTR-NOT: emitrust.struct_def @T {{.*}}has_copy_ctor
struct T : M {
  double top;
};

// The POD sibling: full derive kept, byte-identical to before FR-124.
// CHECK-DAG: emitrust.struct_def @Pod ["a", "b"] [i32, i32]
struct Pod {
  int a;
  int b;
};

// CHECK-DAG: emitrust.struct_def @C ["v"] [i32] {{.*}}emitrust.has_copy_ctor}
struct C {
  int v;
  C() : v(0) {}
  C(const C &o) : v(o.v + 1) {}
};

// Derived over a copy-ctor base: no attribute, non-Copy via the fixpoint.
// CHECK-DAG: emitrust.struct_def @DC ["base", "extra"] [!emitrust.struct<"C">, i32]
// NOATTR-NOT: emitrust.struct_def @DC {{.*}}has_drop
// NOATTR-NOT: emitrust.struct_def @DC {{.*}}has_copy_ctor
struct DC : C {
  int extra;
};

int use_pod(Pod p) { return p.a + p.b; }

int main(int argc, char **) {
  T t;
  t.tag = argc;
  t.mid = 2;
  t.top = 3.5;
  Pod p;
  p.a = argc;
  p.b = 3;
  DC d;
  d.v = argc;
  d.extra = 4;
  printf("%d %d %f %d %d\n", t.tag, t.mid, t.top, use_pod(p), d.v + d.extra);
  return 0;
}

// The emitted derive lines, in declaration order. B and C lose Copy by
// their own attributes (W2.17 / W2.23 -- the seeds); M, T and Dc lose it
// purely through the field fixpoint.
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct B {
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct M {
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct T {
// RUST: #[derive(Clone, Copy, Default)]
// RUST-NEXT: struct Pod {
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct C {
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct Dc {

// The negative pin stated as an absence: no derive line anywhere in the
// emitted crate carries Copy except the POD sibling's.
// RUSTPOD-NOT: #[derive(Clone, Copy
// RUSTPOD: #[derive(Clone, Copy, Default)]
// RUSTPOD-NEXT: struct Pod {
// RUSTPOD-NOT: #[derive(Clone, Copy

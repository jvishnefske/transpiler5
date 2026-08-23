// REQUIRES: cargo
// FR-124: the derive-Copy fixpoint, end to end. THE oracle for the fix:
// the emitted crate's stdout diffed byte for byte against a
// `clang++ -std=c++17` build of the identical source. This is exactly the
// corpus channel measured in the spike: a `virtual ~B() = default` base
// (user-DECLARED dtor -> B carries has_drop and loses Copy) under a
// dtor-less derived D (the inherited walk counts only user-PROVIDED
// dtors, so D carries NO attribute) -- before FR-124 the emitter derived
// Copy over the non-Copy `base: B` field and the crate was exit-0
// UNBUILDABLE at rustc E0204, so this test could not even reach the diff.
//
// What IS admitted and exercised: field access and assignment through the
// newly non-Copy D, and by-value use of the POD control (which must KEEP
// Copy -- the closure may strip nothing without a non-Copy field under
// it). By-value use of D itself is NOT in the subset; that stays a
// located import rejection, pinned in
// test/Import/Cpp/derive-copy-fixpoint-invalid.cpp, with rustc E0382 as
// the loud second-line backstop.
//
// Every value derives from argc (no argv), so no constant folding can
// pre-compute the output and hide a miscompile behind a compile-clean
// crate.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_derive_copy_fixpoint > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct B {
  int tag;
  virtual ~B() = default;
};

struct D : B {
  double weight;
};

struct Pod {
  int a;
  int b;
};

int use_pod(Pod p) { return p.a + p.b; }

int main(int argc, char **) {
  D d;
  d.tag = argc;
  d.weight = 2.5;
  Pod p;
  p.a = argc;
  p.b = 3;
  printf("%d %f %d\n", d.tag, d.weight, use_pod(p));
  return 0;
}

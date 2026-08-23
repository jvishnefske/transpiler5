// REQUIRES: cargo
// W2.19a: virtual methods on VALUES only -- static dispatch, end to end.
// THE oracle for the wave: the emitted crate's stdout is diffed byte for
// byte against a `clang++ -std=c++17` build of the identical source. The
// entire claim is that for a VALUE the C++ static type IS the dynamic
// type, so binding every virtual call to the receiver's own type's
// override -- as an ordinary `emitrust.method_call`, zero new ops, no
// trait, no dyn -- is not an approximation but the exact C++ semantics.
// A compile-clean cargo build cannot see a wrong binding (the W2.19a
// spike measured exactly that miscompile shape when the pointer fence
// was left out); only this diff can.
//
// Covered, all inside the admitted subset (see
// test/Import/Cpp/virtual-methods-values-invalid.cpp for what stays
// rejected): an override bound at every level of a THREE-level hierarchy
// (`calc` -- Base's own body, Mid's override, Leaf's `final` override); a
// virtual method a middle class INHERITS (Mid has no `fixed`, so
// `m.fixed()` must bind Base's body through the W2.18 base-field hop)
// beside the SAME slot overridden one level further down (`l.fixed()`
// binds Leaf's); a plain non-virtual method along the same chain; a
// virtual method inherited across TWO hops (`c.w()` projects
// base.base); and the QUALIFIED spelling `l.Base::calc()`, which C++
// itself binds statically even through pointers -- on a value it must
// agree with the base's body, not Leaf's override.
//
// Every value derives from argc and objects live at FUNCTION scope, so
// no constant folding can pre-compute the answers and hide a miscompile
// behind a compile-clean crate.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_virtual_methods_values > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct Base {
  int seed;
  virtual int calc() { return seed * 2; }
  virtual int fixed() { return 40 + seed; }
  int plain() { return seed - 1; }
};

struct Mid : Base {
  int extra;
  int calc() override { return seed + extra; }
};

struct Leaf : Mid {
  int calc() override final { return extra * 10; }
  int fixed() override { return 7 + seed; }
};

// Virtual method inherited across TWO hops: nothing between the root and
// the leaf touches `w`, so `c.w()` must flatten through base.base.
struct A2 {
  int x;
  virtual int w() { return 5 + x; }
};
struct B2v : A2 {
  int y;
};
struct C2 : B2v {
  int z;
};

int main(int argc, char **) {
  Base b;
  b.seed = argc + 2;
  Mid m;
  m.seed = argc;
  m.extra = argc * 3;
  Leaf l;
  l.seed = argc + 1;
  l.extra = argc + 4;
  printf("%d %d %d\n", b.calc(), b.fixed(), b.plain());
  printf("%d %d %d\n", m.calc(), m.fixed(), m.plain());
  printf("%d %d %d\n", l.calc(), l.fixed(), l.plain());
  printf("%d\n", l.Base::calc());

  C2 c;
  c.x = argc;
  c.y = argc + 1;
  c.z = argc + 2;
  printf("%d %d\n", c.w(), c.z);
  return 0;
}

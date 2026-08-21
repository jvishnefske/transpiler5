// REQUIRES: cargo
// W2.18: single non-virtual inheritance -> the base as an ordinary FIRST
// field, end to end. THE oracle for the wave: the emitted crate's stdout is
// diffed byte for byte against a `clang++ -std=c++17` build of the identical
// source. Rust has no inheritance at all, so every inherited access in this
// file is a REWRITE (`get()` -> `self.base.base_get()`, `x` ->
// `self.base.x`, `Base(a)` -> `self.base.base_new(a)`) -- exactly the class
// of change a compile-clean `cargo build` cannot see.
//
// Covered, all inside the admitted subset (see
// test/Import/Cpp/inheritance-invalid.cpp for what stays rejected):
// a base initializer in the derived constructor, an IMPLICIT inherited
// method call (`get()` with no `this->`), the explicit `this->get()` and
// the qualified `Base::get()` spellings, an inherited field READ and an
// inherited field WRITE, an inherited method called from a derived method
// that also writes an inherited field, a TWO-HOP chain (`Third : Derived :
// Base`, whose projection is `self.base.base`), and a derived object whose
// class declares NO constructor at all -- the shape whose base default
// construction was a measured silent miscompile before this wave (the
// implicit derived default ctor's base initializer used to be skipped, so
// `Nsdmi`/`Defaulted` below printed the Rust zero instead of the base's
// value).
//
// Every value derives from argc, so no constant folding can pre-compute
// the answers and hide a miscompile behind a compile-clean crate.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_inheritance > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct Base {
  int x;
  Base(int v) : x(v) {}
  int get() const { return x; }
  void bump(int d) { x += d; }
};

struct Derived : Base {
  int y;
  Derived(int a, int b) : Base(a), y(b) {}
  int sum() const { return get() + y; }
  int scaled() const { return x * 3 + y; }
  void grow(int d) { x += d; bump(d); y += d; }
  int viaThis() const { return this->get() + this->x; }
  int viaQualified() const { return Base::get(); }
};

struct Third : Derived {
  int z;
  Third(int a, int b, int c) : Derived(a, b), z(c) {}
  int total() const { return get() + x + y + z; }
};

// The measured miscompile channel: a base with a USER-PROVIDED default
// constructor, under a derived class that declares no constructor at all.
// C++ runs `Seed()` as part of the implicit `Holder()`; the base
// initializer lives on the IMPLICIT derived ctor's init list, which the
// default-construction path used to skip in silence (native 9, Rust 2).
struct Seed {
  int s;
  Seed() : s(7) {}
  int seed() const { return s; }
};

struct Holder : Seed {
  int extra;
};

// The same channel through an NSDMI base instead of a user-provided ctor.
struct Nsdmi {
  int n = 9;
};

struct NsdmiDerived : Nsdmi {
  int tail = 1;
};

// The W2.17 interaction, in the one direction the wave admits. C++ destroys
// the DERIVED body first and the base subobject last; Rust runs the
// struct's own `drop()` and then drops its fields in DECLARATION order, and
// `base` is the FIRST field -- so the base drops immediately after the
// derived body, which is exactly C++'s order here. It only stays exactly
// right because W2.17 already forbids every OTHER destructor-carrying
// member, and because W2.18 rejects a base that carries a destructor of its
// own (see test/Import/Cpp/inheritance-invalid.cpp): with both gates in
// force, `base` is the only field whose drop is observable at all.
struct Plain {
  int p;
  Plain(int v) : p(v) {}
  int twice() const { return p * 2; }
};

struct Tracked : Plain {
  int t;
  Tracked(int a, int b) : Plain(a), t(b) {}
  ~Tracked() { printf("drop %d %d\n", p, t); }
};

int main(int argc, char **) {
  Derived d(argc + 2, argc * 4);
  printf("%d\n", d.sum());
  printf("%d\n", d.scaled());
  d.grow(argc + 1);
  printf("%d %d\n", d.get(), d.y);
  printf("%d\n", d.viaThis());
  printf("%d\n", d.viaQualified());
  Third t(argc * 10, argc * 20, argc * 30);
  printf("%d\n", t.total());
  printf("%d\n", t.sum());

  Holder h;
  h.extra = argc * 5;
  printf("%d %d\n", h.seed() + argc, h.s + h.extra);

  NsdmiDerived nd;
  nd.tail += argc;
  printf("%d %d\n", nd.n * argc, nd.tail);

  Tracked first(argc, argc * 2);
  Tracked second(argc * 3, argc * 4);
  printf("%d %d\n", first.twice(), second.twice());
  printf("end\n");
  return 0;
}

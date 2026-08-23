// REQUIRES: cargo
// W2.19b: devirtualized single-object base pointer, end to end. THE
// oracle for the wave: the emitted crate's stdout is diffed byte for
// byte against a `clang++ -std=c++17` build of the identical source. The
// claim is that a pointer whose region binds EXACTLY ONE object has that
// object's dynamic type statically, so resolving a VIRTUAL call to the
// object's own final overrider -- as an ordinary `emitrust.method_call`,
// zero new ops, no trait, no dyn -- is exact C++ semantics. A
// compile-clean cargo build CANNOT see a wrong binding: every wrong
// resolution below (Der::calc vs Base::calc, Base::tag vs Der::tag,
// Base::fixed reached without the hop) compiles clean and prints
// different numbers; only this diff can catch it.
//
// The legD4 adversarial core: through ONE base pointer, a VIRTUAL call
// must resolve by the OBJECT's dynamic type (`p->calc()` -> Der::calc)
// while a NON-virtual call must resolve by the POINTER's static type
// (`p->tag()` -> Base::tag, though Der declares its own `tag` -- the
// value call `d.tag()` prints Der's answer beside it). Both rules ship
// in this wave and both are pinned by the same run. Also covered: an
// inherited virtual devirtualized THROUGH the base hop (`p->fixed()` --
// Der has no override, so the target is Base's own body on `d.base`); a
// SAME-TYPE local pointer virtual call (the W2.19a carve-out this wave
// flips); the 00901 corpus shape (virtual dtor PLUS a const virtual
// method, devirt through `Shape *`); and a DROPPY chain under a bound
// base pointer -- the pointer does not own, so `~RB` must run exactly
// once, AFTER "end", with the base-pointer mutation visible in the
// destructor's output (drop timing is the one channel a wrong ownership
// model would shift).
//
// Every value derives from argc and objects live at function scope, so
// constant folding cannot pre-compute the answers and hide a miscompile
// behind a compile-clean crate.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_devirt_base_pointer > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct Base {
  int seed;
  virtual int calc() { return seed * 2; }
  virtual int fixed() { return 40 + seed; }
  int tag() { return 100 + seed; }
};

struct Der : Base {
  int extra;
  int calc() override { return seed + extra; }
  int tag() { return 200 + seed; }
};

// The 00901 shape: virtual destructor AND a const virtual method.
struct Shape {
  virtual int sides() const { return 0; }
  virtual ~Shape() {}
};
struct Tri : Shape {
  int sides() const override { return 3; }
};

struct S {
  int k;
  virtual int f() { return 1 + k; }
};

// Droppy chain under a bound base pointer: the mutation through the
// pointer must be visible in the destructor's output, and the drop must
// run at the VALUE's scope exit (after "end"), exactly once.
struct RB {
  int v;
  RB(int i) : v(i) {}
  int get() const { return v; }
  void add(int d) { v += d; }
  virtual ~RB() { printf("~RB %d\n", v); }
};
struct RD : RB {
  RD(int i) : RB(i) {}
};

int main(int argc, char **) {
  Der d;
  d.seed = argc;
  d.extra = argc * 3;
  Base *p = &d;
  // Virtual-by-dynamic-type vs non-virtual-by-static-type through the
  // SAME pointer, with the value call's Der::tag answer beside it.
  printf("%d %d %d %d\n", p->calc(), p->fixed(), p->tag(), d.tag());

  Tri t;
  Shape *s = &t;
  printf("%d %d\n", s->sides(), t.sides());

  S sv;
  sv.k = argc + 4;
  S *q = &sv;
  printf("%d %d\n", q->f(), (*q).f());

  RD rd(argc + 7);
  RB *pr = &rd;
  pr->add(argc);
  printf("%d\n", pr->get());
  printf("end\n");
  return 0;
}

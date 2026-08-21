// REQUIRES: cargo
// W2.17: user-declared destructors -> `impl Drop`, end to end. THE oracle
// for the wave: the emitted crate's stdout is diffed byte for byte against
// a `clang++ -std=c++17` build of the identical source. A destructor that
// printf()s makes DROP ORDER directly observable, so this differential is
// not vacuous -- it is the only thing that can see the reverse-declaration
// order, the per-iteration loop-body drop, and the branch-body drop that
// the wave claims to reproduce.
//
// Covered, all inside the admitted subset (see
// test/Import/Cpp/destructors-invalid.cpp for what stays rejected):
// several locals in one function scope (destroyed in REVERSE declaration
// order), a loop-body local (constructed and destroyed once per
// iteration), an if-branch local, an early `return` (which the importer
// structurizes into if/else + a tail expression, so the drop points must
// survive that rewrite), a field-less RAII guard class, and a W2.16 class
// template carrying a destructor (monomorphized, so the Drop impl must be
// emitted per instantiation).
//
// Every value derives from argc, so no constant folding can pre-compute
// the answers and hide a miscompile behind a compile-clean crate.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_destructor > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct R {
  int id;
  R(int i) : id(i) { printf("ctor %d\n", id); }
  void bump(int d) { id += d; printf("bump -> %d\n", id); }
  int get() const { return id; }
  ~R() { printf("dtor %d\n", id); }
};

// A field-less guard: no data, pure RAII.
struct Guard {
  Guard() { printf("guard ctor\n"); }
  ~Guard() { printf("guard dtor\n"); }
};

// A destructor DEFINED OUT OF LINE, in this same translation unit: the
// FR-47 signature prepass leaves a body-less stub in the class walk, which
// the top-level walk then fills in when it reaches `Outl::~Outl()`.
struct Outl {
  int id;
  Outl(int i) : id(i) { printf("outl ctor %d\n", id); }
  ~Outl();
};

// A member function literally spelled `drop`. It already owns the module
// symbol `<Struct>_drop`, which is exactly why the destructor takes
// `<Struct>_dtor` instead -- if the two collided, one of them would be
// silently lost. Both must be callable/observable here.
struct Manual {
  int id;
  Manual(int i) : id(i) { printf("manual ctor %d\n", id); }
  void drop() { printf("manual drop %d\n", id); }
  ~Manual() { printf("manual dtor %d\n", id); }
};

template <typename T>
struct Box {
  T v;
  Box(T x) : v(x) { printf("box ctor %d\n", (int)v); }
  ~Box() { printf("box dtor %d\n", (int)v); }
};

// Reverse-declaration-order drop of three function-scope locals, plus a
// loop-body local that is created and destroyed on every iteration.
static int scoped(int n) {
  R a(n);
  R b(n + 10);
  a.bump(1);
  b.bump(2);
  for (int i = 0; i < 3; i++) {
    R c(100 + i + n);
    c.bump(i);
  }
  R d(n + 20);
  d.bump(3);
  printf("scoped end %d\n", a.get() + b.get() + d.get());
  return a.get() + b.get() + d.get();
}

Outl::~Outl() { printf("outl dtor %d\n", id); }

// A destructor-carrying object reached BY REFERENCE: a borrow never moves,
// so the callee must not drop it and the caller must.
static int look(const R &r) { return r.get(); }

// Both arms of an if/else declare their own object, so the two branch
// bodies must each be a real Rust block.
static int branchy(int n) {
  if (n > 0) {
    R t(n + 50);
    t.bump(6);
    return t.get();
  } else {
    R u(n + 60);
    u.bump(7);
    return u.get();
  }
}

// An early return: the importer structurizes it into if/else plus a tail
// expression, and the drop of `e` must still happen on BOTH paths.
static int early(int n) {
  R e(n + 30);
  if (n > 0) {
    R f(n + 40);
    f.bump(4);
    return e.get() + f.get();
  }
  e.bump(5);
  return e.get();
}

int main(int argc, char **argv) {
  Guard g;
  int s = scoped(argc);
  int e = early(argc);
  int br = branchy(argc);
  Outl o(argc + 70);
  Manual m(argc + 80);
  m.drop();
  R seen(argc + 90);
  int lk = look(seen);
  Box<int> bi(argc + 7);
  Box<long> bl(argc + 8);
  printf("s=%d e=%d br=%d lk=%d bi=%d bl=%d\n", s, e, br, lk, bi.v, (int)bl.v);
  return 0;
}

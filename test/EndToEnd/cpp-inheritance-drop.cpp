// REQUIRES: cargo
// W2.26: polymorphic RAII, value-only subset -- transitive drop through
// inheritance chains, end to end. THE oracle for the wave: the emitted
// crate's stdout is diffed byte for byte against a `clang++ -std=c++17`
// build of the identical source. Destructors printf(), so DROP ORDER is
// directly observable -- and drop order is the entire claim: a
// merely-inheriting derived class emits NO `impl Drop` of its own, and
// `~Base` must still run exactly once, at the same program point as C++,
// through Rust's field-drop glue alone. A compile-clean cargo build
// cannot see any of that.
//
// Covered, all inside the admitted subset (see
// test/Import/Cpp/inheritance-drop-invalid.cpp for what stays rejected):
// a ONE-LEVEL merely-inheriting derived class (`~B` once, after the
// body); a THREE-LEVEL chain whose only destructor is at the ROOT under
// a dtor-less middle and leaf (`~A` once -- the transitive predicate must
// recurse the whole chain); a derived class WITH its own destructor
// (`~E` then `~B`: Rust runs the drop body then the fields, which is
// exactly C++'s derived-body-then-base order); a MIXED chain with a
// droppy middle over a droppy root under a dtor-less leaf (`~M` then
// `~A`); the EMPTY droppy base materialized as a zero-field struct
// (`~Shape` exactly once -- the base whose destructor was LOST outright
// under the old empty-base skip); and a class whose sole virtual member
// is its destructor, held as a value (`~V`).
//
// Every value derives from argc and objects live at FUNCTION scope only
// (a bare block is itself a rejection), so no constant folding can
// pre-compute the answers and hide a miscompile behind a compile-clean
// crate.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_inheritance_drop > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct B {
  int x;
  B(int v) : x(v) { printf("ctor B %d\n", x); }
  int get() const { return x; }
  ~B() { printf("~B %d\n", x); }
};

// One level, merely inheriting: no ~D, so only the base's drop runs.
struct D : B {
  int y;
  D(int a, int b) : B(a), y(b) { printf("ctor D %d\n", y); }
  int sum() const { return get() + y; }
};

// Derived WITH its own destructor: body first, then the base field.
struct E : B {
  int z;
  E(int a, int b) : B(a), z(b) { printf("ctor E %d\n", z); }
  ~E() { printf("~E %d\n", z); }
};

// Three levels, the ONLY destructor at the root.
struct A {
  int a;
  A(int v) : a(v) { printf("ctor A %d\n", a); }
  ~A() { printf("~A %d\n", a); }
};
struct Mid : A {
  int m;
  Mid(int v, int w) : A(v), m(w) {}
};
struct Leaf : Mid {
  int l;
  Leaf(int v, int w, int u) : Mid(v, w), l(u) {}
};

// Mixed chain: a droppy middle over the droppy root, dtor-less leaf.
struct MidD : A {
  int m;
  MidD(int v, int w) : A(v), m(w) {}
  ~MidD() { printf("~M %d\n", m); }
};
struct LeafM : MidD {
  int l;
  LeafM(int v, int w, int u) : MidD(v, w), l(u) {}
};

// The empty droppy base, materialized.
struct Shape {
  ~Shape() { printf("~Shape\n"); }
};
struct Circle : Shape {
  int r;
  Circle(int v) : r(v) { printf("ctor Circle %d\n", r); }
};

// Sole-virtual-destructor class as a value.
struct V {
  int id;
  V(int i) : id(i) { printf("ctor V %d\n", id); }
  virtual ~V() { printf("~V %d\n", id); }
};

static int one_level(int n) {
  D d(n, n + 1);
  printf("body D %d\n", d.sum());
  return d.y;
}

static int own_dtor(int n) {
  E e(n + 2, n + 3);
  printf("body E %d\n", e.z);
  return e.z;
}

static int three_level(int n) {
  Leaf l(n + 4, n + 5, n + 6);
  printf("body Leaf %d\n", l.l);
  return l.l;
}

static int mixed(int n) {
  LeafM t(n + 7, n + 8, n + 9);
  printf("body LeafM %d\n", t.l);
  return t.l;
}

static int empty_base(int n) {
  Circle c(n + 10);
  printf("body Circle %d\n", c.r);
  return c.r;
}

static int virt(int n) {
  V v(n + 11);
  printf("body V %d\n", v.id);
  return v.id;
}

// Two droppy-chain locals in ONE scope: reverse declaration order across
// classes from different chains.
static int interleaved(int n) {
  D d(n + 12, n + 13);
  E e(n + 14, n + 15);
  printf("body interleaved %d\n", d.y + e.z);
  return d.y + e.z;
}

int main(int argc, char **argv) {
  int s = one_level(argc);
  s += own_dtor(argc);
  s += three_level(argc);
  s += mixed(argc);
  s += empty_base(argc);
  s += virt(argc);
  s += interleaved(argc);
  printf("sum %d\n", s);
  return 0;
}

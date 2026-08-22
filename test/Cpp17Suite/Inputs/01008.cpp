// Cpp17Suite 01008: polymorphic RAII, value-only subset (W2.26) -- a base
// class with a destructor under derived classes WITH and WITHOUT their
// own, at one, two and three levels. Deterministic stdout that makes the
// transitive drop observable: a merely-inheriting derived class emits no
// `impl Drop` of its own, so `~Base` printing exactly once, at C++'s
// program point, is carried entirely by Rust's field-drop glue; a derived
// destructor printing BEFORE its base's pins the derived-body-then-base
// order; and the empty droppy base pins the ~Shape line the old
// empty-base skip used to lose outright.
extern "C" int printf(const char *, ...);

struct Base {
  int x;
  Base(int v) : x(v) { printf("ctor Base %d\n", x); }
  int get() const { return x; }
  ~Base() { printf("~Base %d\n", x); }
};

// No destructor of its own: only ~Base runs, once.
struct Plain : Base {
  int y;
  Plain(int a, int b) : Base(a), y(b) {}
  int sum() const { return get() + y; }
};

// Its own destructor: body first, then the base subobject.
struct Loud : Base {
  int z;
  Loud(int a, int b) : Base(a), z(b) {}
  ~Loud() { printf("~Loud %d\n", z); }
};

// Three levels, destructors at the root and the middle only.
struct Mid : Base {
  int m;
  Mid(int a, int b) : Base(a), m(b) {}
  ~Mid() { printf("~Mid %d\n", m); }
};
struct Leaf : Mid {
  int l;
  Leaf(int a, int b, int c) : Mid(a, b), l(c) {}
};

// The empty droppy base.
struct Shape {
  ~Shape() { printf("~Shape\n"); }
};
struct Circle : Shape {
  int r;
  Circle(int v) : r(v) {}
};

static int plain_scope(int n) {
  Plain p(n, n + 1);
  printf("plain body %d\n", p.sum());
  return p.y;
}

static int loud_scope(int n) {
  Loud q(n + 2, n + 3);
  printf("loud body %d\n", q.z);
  return q.z;
}

static int leaf_scope(int n) {
  Leaf f(n + 4, n + 5, n + 6);
  printf("leaf body %d\n", f.l);
  return f.l;
}

static int circle_scope(int n) {
  Circle c(n + 7);
  printf("circle body %d\n", c.r);
  return c.r;
}

int main() {
  int s = plain_scope(1);
  s += loud_scope(2);
  s += leaf_scope(3);
  s += circle_scope(4);
  printf("sum=%d\n", s);
  return 0;
}

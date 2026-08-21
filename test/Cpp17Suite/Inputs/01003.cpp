// Cpp17Suite 01003: RAII -- user-declared destructors lowered to Rust's
// `impl Drop`. Deterministic stdout that makes DROP ORDER observable: a
// destructor printf()s, so reverse-declaration-order destruction of
// function-scope locals, per-iteration destruction of a loop-body local,
// destruction of an if-branch local, and destruction of a class-template
// instantiation's object at end of main are all visible in the diff
// against the committed clang++ -std=c++17 reference output.
extern "C" int printf(const char *, ...);

struct Tracer {
  int id;
  Tracer(int i) : id(i) { printf("ctor %d\n", id); }
  void bump(int d) { id += d; printf("bump %d\n", id); }
  ~Tracer() { printf("dtor %d\n", id); }
};

template <typename T>
struct Guard {
  T v;
  Guard(T x) : v(x) { printf("guard ctor %d\n", (int)v); }
  ~Guard() { printf("guard dtor %d\n", (int)v); }
};

static int scoped(int n) {
  Tracer a(n);
  a.bump(1);
  for (int i = 0; i < 2; i++) {
    Tracer c(100 + i);
    c.bump(i);
  }
  if (n > 1) {
    Tracer t(900);
    t.bump(9);
  }
  Tracer b(n + 10);
  b.bump(2);
  return a.id + b.id;
}

int main() {
  printf("scoped=%d\n", scoped(2));
  Guard<int> g(7);
  printf("done\n");
  return 0;
}

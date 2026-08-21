// Cpp17Suite 01002: class-template monomorphization -- one class template
// with a constructor and a const method, instantiated at two distinct
// types (int and double), plus a two-parameter template instantiated at
// both argument orders (so the suffix codes must join in template-
// parameter declaration order).
extern "C" int printf(const char *, ...);

template <typename T>
struct Box {
  T v;
  Box(T x) : v(x) {}
  T get() const { return v; }
  void bump(T d) { v = v + d; }
};

template <typename A, typename B>
struct Pair2 {
  A a;
  B b;
  Pair2(A x, B y) : a(x), b(y) {}
  A first() const { return a; }
  B second() const { return b; }
};

int main() {
  Box<int> bi(7);
  Box<double> bd(1.25);
  bi.bump(3);
  bd.bump(0.25);
  Pair2<int, double> p(4, 2.5);
  Pair2<double, int> q(0.5, 9);
  printf("%d %.2f %d %.2f %.2f %d\n", bi.get(), bd.get(), p.first(),
         p.second(), q.first(), q.second());
  return 0;
}

// REQUIRES: cargo
// W2.16: a class-template monomorphization differential, end to end.
// One `Box<T>` (constructor with a member-initializer list, a const
// accessor, a mutating method) is instantiated at `int`, `long`,
// `double`, `float` and `char`; one two-type-parameter `Pair2<A, B>` at
// BOTH argument orders, which is the shape that dies if the suffix codes
// are concatenated in anything but template-parameter declaration order;
// and a free function takes one instantiation BY VALUE and another BY
// REFERENCE, so both parameter forms of an instantiated type are covered.
// Every value derives from argc, so constant folding cannot pre-compute
// the answers and hide a miscompile behind a compile-clean crate.
// Byte-identical vs `clang++ -std=c++17`.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_class_template > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

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

int sum_boxes(Box<int> x, Box<int> &y) { return x.get() + y.get(); }

int main(int argc, char **argv) {
  int seed = argc; /* 1 at run time, opaque to the folder */
  Box<int> bi(seed + 6);
  Box<long> bl((long)seed * 1000000000L);
  Box<double> bd(1.25 * seed);
  Box<float> bf(0.5f * seed);
  Box<char> bc((char)(64 + seed));
  bi.bump(seed + 2);
  bd.bump(0.25 * seed);
  Pair2<int, double> p(seed + 4, 2.5 * seed);
  Pair2<double, int> q(0.5 * seed, seed + 9);
  Box<int> other(seed + 11);
  printf("%d %ld %.4f %.4f %d %d %.4f %.4f %d\n", bi.get(), bl.get(), bd.get(),
         (double)bf.get(), (int)bc.get(), p.first(), p.second(), q.first(),
         q.second());
  printf("%d\n", sum_boxes(bi, other));
  return 0;
}

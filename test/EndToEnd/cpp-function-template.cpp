// REQUIRES: cargo
// W2.15: a function-template monomorphization differential, end to end.
// One `add<T>` is instantiated at `int`, `long`, `double` and `float`;
// one `scale<T>` (whose own body CALLS `add<T>`, so the inner call must
// resolve to the SAME instantiation) at `int` and `double`; a two-type-
// parameter `cvt<A, B>` at both argument orders, which is the shape that
// dies if the suffix codes are concatenated in anything but template-
// parameter declaration order. Every value derives from argc, so constant
// folding cannot pre-compute the answers and hide a miscompile behind a
// compile-clean crate. Byte-identical vs `clang++ -std=c++17`.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_function_template > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

template <typename T>
T add(T a, T b) {
  return a + b;
}

template <typename T>
T scale(T a, int n) {
  T total = a;
  for (int i = 1; i < n; ++i)
    total = add(total, a);
  return total;
}

template <typename A, typename B>
int cvt(A a, B b) {
  return (int)a - (int)b;
}

int main(int argc, char **argv) {
  int seed = argc;               /* 1 at run time, opaque to the folder */
  int i = add(seed + 1, seed + 2);
  long l = add((long)seed * 1000000000L, (long)seed);
  double d = add(1.25 * seed, 2.5);
  float f = add(0.5f * seed, 0.25f);
  int si = scale(seed + 3, seed + 2);
  double sd = scale(0.5 * seed, seed + 2);
  int c1 = cvt(seed + 7, 2.5);
  int c2 = cvt(2.5, seed + 7);
  printf("%d %ld %.4f %.4f %d %.4f %d %d\n", i, l, d, (double)f, si, sd, c1,
         c2);
  return 0;
}

// REQUIRES: cargo
// W2.0: C++ input (subset) differential end-to-end test. Exercises the
// same C++-frontend AST tolerance test/Import/Cpp/cpp-basics.cpp pins at
// the IR level end to end through a built Rust binary: a namespace
// (flattened symbol), a nested namespace, extern "C" linkage, bool, and a
// plain data-only struct/class with access specifiers. main returns 0 and
// reports everything via printf, so lit's per-command exit-code checking
// covers both runs and diff covers the observable behavior. The native
// leg is built with clang++ (not clang), since the source is compiled as
// C++; printf is declared `extern "C"` so the native C++ build links
// against libc's printf instead of mangling a C++-linkage lookup for it.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_basics > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

namespace shapes {
int area(int w, int h) {
  return w * h;
}

int base_offset = 100;

namespace detail {
int adjust(int x) {
  return x - 3;
}
} // namespace detail
} // namespace shapes

extern "C" {
int c_linked_add(int a, int b) {
  return a + b;
}
}

struct Point {
public:
  int x;
  int y;
};

class Pair {
public:
  int a;
  int b;
};

bool is_even(int x) {
  bool result = (x % 2) == 0;
  return result;
}

int main(void) {
  Point p;
  p.x = 3;
  p.y = 4;

  Pair pr;
  pr.a = 7;
  pr.b = 8;

  int total = shapes::area(p.x, p.y) + shapes::base_offset;
  total = total + shapes::detail::adjust(total);
  total = total + c_linked_add(pr.a, pr.b);

  printf("area+offset+adjust+sum=%d\n", total);
  printf("is_even(4)=%d\n", is_even(4));
  printf("is_even(5)=%d\n", is_even(5));
  printf("point=%d,%d pair=%d,%d\n", p.x, p.y, pr.a, pr.b);
  return 0;
}

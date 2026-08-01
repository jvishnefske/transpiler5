// REQUIRES: cargo
// Broad-capture regression: std::vector<user-struct> end to end. Widens the
// std::vector element-type coverage (stl-vector.cpp pins vector<int>) to a
// user data struct -- push_back of a whole struct value and subscript member
// access. Byte-matches the clang++-native build; main returns 0 and reports
// via printf.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_vector_struct > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);
#include <vector>

struct P {
  int x;
  int y;
};

int main() {
  std::vector<P> v;
  P a;
  a.x = 1;
  a.y = 2;
  v.push_back(a);
  P b;
  b.x = 3;
  b.y = 4;
  v.push_back(b);
  int s = 0;
  for (unsigned i = 0; i < v.size(); i++)
    s += v[i].x + v[i].y;
  printf("%d %zu\n", s, v.size());
  return 0;
}

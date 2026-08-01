// REQUIRES: cargo
// Broad-capture regression: std::vector<double> end to end. stl-vector.cpp
// already pins vector<int>; this widens the element-type coverage to a
// floating element (push_back, size, and subscript). Byte-matches the
// clang++-native build; main returns 0 and reports via printf.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_vector_double > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);
#include <vector>

int main() {
  std::vector<double> v;
  v.push_back(1.5);
  v.push_back(2.25);
  v.push_back(-0.75);
  double s = 0;
  for (unsigned i = 0; i < v.size(); i++)
    s += v[i];
  printf("%.2f %zu\n", s, v.size());
  return 0;
}

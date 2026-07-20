// REQUIRES: cargo
// W2.3: std::vector<T> recognition, end to end. The real oracle for the
// vector half of the wave: builds a Rust crate through the full import +
// conversion + Rust-emission + `cargo build --release` pipeline and diffs
// its stdout against a `clang++ -std=c++17` build of the identical source,
// byte for byte. Exercises: default construction, push_back, size(),
// operator[], at(), empty(), clear(), across two independently-seeded
// vectors (distinct values at every call site, so a transpiler wiring bug
// — the wrong argument reaching the wrong parameter/receiver — would show
// up as a mismatch rather than being hidden by a repeated literal).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_vector > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <vector>

int use_vector(int seed) {
  std::vector<int> v;
  v.push_back(seed);
  v.push_back(seed * 2);
  v.push_back(seed * 3);
  int n = v.size();
  int first = v[0];
  int second = v.at(1);
  int third = v[2];
  int empty_before = v.empty();
  v.clear();
  int empty_after = v.empty();
  return n + first + second + third + empty_before * 100 + empty_after * 1000;
}

int main(void) {
  int a = use_vector(7);
  int b = use_vector(11);
  printf("%d %d\n", a, b);
  return 0;
}

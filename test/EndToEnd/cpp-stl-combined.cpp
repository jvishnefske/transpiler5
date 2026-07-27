// REQUIRES: cargo
// W4: std::vector<int> and std::string co-resident in one TU, plus the
// untested `.length()` spelling, end to end. The existing STL tests split
// vector and string across separate crates; this builds one crate through
// the full import + conversion + Rust-emission + `cargo build --release`
// pipeline that uses both opaque STL paths in the same function, and diffs
// its stdout against a `clang++ -std=c++17` build of the identical source,
// byte for byte. Exercises: vector default ctor, push_back, size(),
// operator[], at(), empty(), clear() alongside a string-literal-ctor
// std::string with `+=` char, `+=` literal, length() (the sibling of the
// already-tested size()), empty(), and c_str() fed straight to printf's
// '%s'. Called twice with distinct seeds so the seed-dependent vector
// values would surface a wiring bug as a diff mismatch.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_stl_combined > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <vector>
#include <string>

int summarize(int seed) {
  std::vector<int> v;
  v.push_back(seed);
  v.push_back(seed + 1);
  v.push_back(seed * 2);
  int n = v.size();
  int first = v[0];
  int last = v.at(2);
  int empty_before = v.empty();
  v.clear();
  int empty_after = v.empty();

  std::string s = "tag=";
  s += '#';
  s += " ok";
  int len = s.length();
  int se = s.empty();

  printf("%s seed=%d n=%d first=%d last=%d eb=%d ea=%d len=%d se=%d\n",
         s.c_str(), seed, n, first, last, empty_before, empty_after, len, se);
  return n + first + last + len;
}

int main(void) {
  int a = summarize(4);
  int b = summarize(10);
  printf("%d %d\n", a, b);
  return 0;
}

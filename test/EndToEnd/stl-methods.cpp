// REQUIRES: cargo
// W2.6: vector front()/back()/pop_back() and string push_back(char)/
// clear(), end to end. Builds a Rust crate through the full import +
// conversion + Rust-emission + `cargo build --release` pipeline and diffs
// its stdout against a `clang++ -std=c++17` build of the identical
// source, byte for byte. front/back read through subscript places
// (`v[0]` / `v[v.len() - 1]`); pop_back is a discarded `pop` — every call
// here is on a non-empty container, the only shape defined in C++ (empty
// front/back/pop_back is UB, where the Rust spellings panic or no-op
// rather than yield wrong data). back() is re-read after pop_back so the
// pop's length change is observable, and the whole sequence runs on two
// independently-seeded vectors so a wiring bug cannot hide behind a
// repeated literal.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_methods > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <vector>
#include <string>

int drain(int seed) {
  std::vector<int> v;
  v.push_back(seed);
  v.push_back(seed * 2);
  v.push_back(seed * 5);
  int f = v.front();
  int b = v.back();
  v.pop_back();
  int b2 = v.back();
  v.pop_back();
  int n = v.size();
  return f * 1000 + b * 10 + b2 + n;
}

int main(void) {
  std::string s = "id:";
  s.push_back('a');
  s.push_back('7');
  int len = s.length();
  printf("%s %d %d %d\n", s.c_str(), len, drain(3), drain(8));
  s.clear();
  int empty_after = s.empty();
  int len_after = s.length();
  printf("%d %d\n", empty_after, len_after);
  return 0;
}

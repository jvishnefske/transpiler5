// REQUIRES: cargo
// W2.8: std::pair<T1, T2>, end to end. Builds a Rust crate through the
// full import + conversion + Rust-emission + `cargo build --release`
// pipeline and diffs its stdout against a `clang++ -std=c++17` build of
// the identical source, byte for byte. Exercises: two-argument value
// construction, .first/.second reads and writes (every write changes the
// stored value, so a dropped store shows as a diff), a pair returned by
// value from a factory (C++17 guaranteed elision on the native leg; the
// transpiled leg's temp-and-return models the same single construction),
// a mixed-element pair<int, double> printed with %.2f, and two
// independently-seeded factory calls so a wiring bug cannot hide behind a
// repeated literal.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_pair > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <utility>

std::pair<int, int> divmod(int num, int den) {
  return std::pair<int, int>(num / den, num % den);
}

int main(void) {
  std::pair<int, int> p(3, 40);
  p.first += 2;
  p.second = p.second + p.first;
  std::pair<int, int> a = divmod(17, 5);
  std::pair<int, int> b = divmod(23, 4);
  std::pair<int, double> mixed(2, 0.5);
  mixed.second = mixed.second * 3.0 + mixed.first;
  printf("%d %d %d %d %d %d %d %.2f\n", p.first, p.second, a.first,
         a.second, b.first, b.second, mixed.first, mixed.second);
  return 0;
}

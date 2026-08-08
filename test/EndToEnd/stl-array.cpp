// REQUIRES: cargo
// W2.7: std::array<T, N>, end to end. Builds a Rust crate through the
// full import + conversion + Rust-emission + `cargo build --release`
// pipeline and diffs its stdout against a `clang++ -std=c++17` build of
// the identical source, byte for byte. Exercises: aggregate init through
// the struct-wrapper peel, subscript READ and WRITE (every written value
// differs from the value it overwrites, so a dropped store shows as a
// diff), size() folding to the constant N, a double-element array with
// %.2f-pinned output, and a runtime (non-constant) subscript index fed
// from a parameter across two calls with distinct seeds.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_array > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <array>

int mix(int seed) {
  std::array<int, 4> a = {seed, seed * 2, seed + 9, 1};
  a[3] = a[0] + a[2];
  a[seed % 4] = a[3] * 10;
  int n = a.size();
  return a[0] + a[1] + a[2] + a[3] + n;
}

int main(void) {
  std::array<double, 3> d = {1.5, 2.25, 4.0};
  d[1] = d[1] * 2.0 + d[0];
  printf("%.2f %.2f %d %d\n", d[1], d[0] + d[1] + d[2], mix(3), mix(6));
  return 0;
}

// REQUIRES: cargo
// W2.9: by-value structured bindings, end to end. Builds a Rust crate
// through the full import + conversion + Rust-emission + `cargo build
// --release` pipeline and diffs its stdout against a `clang++ -std=c++17`
// build of the identical source, byte for byte. Exercises all three
// source shapes (user struct, std::pair — including one returned by value
// from a factory, and std::array), plus the copy semantics the desugar
// must preserve: after `auto [w, h] = d;` a WRITE to the binding must not
// leak back into `d` (the binding is a member of the hidden copy), which
// this pins by printing both the mutated binding and the untouched
// source field.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_structured_bindings > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <utility>
#include <array>

struct Dim {
  int w;
  int h;
};

std::pair<int, int> divmod(int num, int den) {
  return std::pair<int, int>(num / den, num % den);
}

int main(void) {
  Dim d;
  d.w = 12;
  d.h = 5;
  auto [w, h] = d;
  w = w + 100;
  printf("%d %d %d %d\n", w, h, d.w, d.h);

  auto [q, r] = divmod(23, 5);
  printf("%d %d\n", q, r);

  std::array<int, 3> a = {7, 21, 35};
  auto [x, y, z] = a;
  z = z - 30;
  printf("%d %d %d %d\n", x, y, z, a[2]);
  return 0;
}

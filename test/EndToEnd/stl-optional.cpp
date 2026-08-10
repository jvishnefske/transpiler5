// REQUIRES: cargo
// W2.11: std::optional<int>, end to end. Builds a Rust crate through the
// full import + conversion + Rust-emission + `cargo build --release`
// pipeline and diffs its stdout against a `clang++ -std=c++17` build of
// the identical source, byte for byte. Every seed derives from argc so
// constant folding cannot hide a miscompile: the engaged optional
// (`find_even(seed)`, seed always even) must report has_value()==1 and
// value_or() its payload; the empty one (`find_even(seed - 1)`, always
// odd -> std::nullopt) must report 0 and the DEFAULT — with a
// runtime-computed default so unwrap_or's argument wiring is exercised
// too; a default-constructed optional pins the `None` local-init path.
// A second run with one extra argument shifts every seed, so the two
// legs must agree on two distinct input vectors.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_optional > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native extra > %t.native.out
// RUN: %t.crate/target/release/stl_optional extra > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <optional>

std::optional<int> find_even(int v) {
  if (v % 2 == 0)
    return v;
  return std::nullopt;
}

int main(int argc, char **argv) {
  int seed = argc * 8; // 8 on the bare run, 16 with one argument
  std::optional<int> a = find_even(seed);     // engaged: Some(seed)
  std::optional<int> b = find_even(seed - 1); // odd: None
  printf("%d %d %d %d\n", (int)a.has_value(), a.value_or(-1),
         (int)b.has_value(), b.value_or(-1));
  // Runtime-computed unwrap_or default, and the default-constructed
  // (`None`) local-initialization path.
  std::optional<int> o;
  printf("%d %d %d\n", (int)o.has_value(), o.value_or(seed + 3),
         b.value_or(seed * 2));
  return 0;
}

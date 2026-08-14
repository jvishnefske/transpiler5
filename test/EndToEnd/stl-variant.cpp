// REQUIRES: cargo
// W2.14: std::variant<int, double>, end to end. Builds a Rust crate
// through the full import + conversion + Rust-emission + `cargo build
// --release` pipeline and diffs its stdout against a `clang++ -std=c++17`
// build of the identical source, byte for byte. Every seed derives from
// argc so constant folding cannot hide a miscompile: the synthesized
// data-enum image must report index() 0/1 correctly across BOTH
// alternatives, std::get<T> must yield the runtime-held payload through
// its match expansion (the wrong-alternative panic arms compile in but
// must never fire), operator= must genuinely re-tag the variant in both
// directions (int -> double -> int), and the default-constructed variant
// pins the explicit V0{0} first-alternative image. A second run with one
// extra argument shifts every seed, so the two legs must agree on two
// distinct input vectors.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_variant > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native extra > %t.native.out
// RUN: %t.crate/target/release/stl_variant extra > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <variant>

int main(int argc, char **argv) {
  int seed = argc * 7; // 7 on the bare run, 14 with one argument
  std::variant<int, double> v = seed; // converting ctor: holds int
  printf("%d %d\n", (int)v.index(), std::get<int>(v));
  v = seed + 0.5; // re-tag to the double alternative
  printf("%d %.1f\n", (int)v.index(), std::get<double>(v));
  v = seed * 3; // and back to int
  printf("%d %d\n", (int)v.index(), std::get<int>(v));
  std::variant<int, double> w; // default ctor: holds int 0
  printf("%d %d\n", (int)w.index(), std::get<int>(w) + seed);
  return 0;
}

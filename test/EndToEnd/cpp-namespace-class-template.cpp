// REQUIRES: cargo
// FR-108, the leg that SUPERSEDES W2.16's `COLNS` rejection. W2.16 could
// only DIAGNOSE `::Box<int>` beside `ns::Box<int>` (both composed the one
// spelling `Box_i32` from two different patterns, so the second silently
// took the first's method bodies — native `1 101`, crate `1 1`). With
// FR-108's namespace prefix on record names the two COEXIST instead:
// `BoxI32` and `NsNsBoxI32`, each with its own `get`. This file is the
// direct evidence that the rejection was superseded rather than dropped,
// so `test/Import/Cpp/class-templates-invalid.cpp`'s COLNS section was
// retired in the same change.
//
// Pins that the prefix composes with the W2.16 template-argument suffix
// BEFORE the idiomatic camel fold (`ns::Box<int>` -> `NsNsBoxI32`, not
// `NsNsBox_i32`, which rustc's denied non_camel_case_types lint would
// reject), at two argument types and through a nested namespace. Every
// value derives from argc, so constant folding cannot hide a miscompile.
// Byte-identical vs `clang++ -std=c++17`.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_namespace_class_template > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

template <typename T>
struct Box {
  T v;
  Box(T x) : v(x) {}
  T get() const { return v; }
};

namespace ns {
template <typename T>
struct Box {
  T v;
  Box(T x) : v(x) {}
  T get() const { return v + 100; }
};
} // namespace ns

namespace a {
namespace b {
template <typename T>
struct Box {
  T v;
  Box(T x) : v(x) {}
  T get() const { return v + 7; }
};
} // namespace b
} // namespace a

int main(int argc, char **argv) {
  int seed = argc; /* 1 at run time, opaque to the folder */
  Box<int> g(seed);
  ns::Box<int> n(seed);
  Box<double> gd(0.5 * seed);
  ns::Box<double> nd(0.5 * seed);
  a::b::Box<int> deep(seed * 3);
  printf("%d %d %.4f %.4f %d\n", g.get(), n.get(), gd.get(), nd.get(),
         deep.get());
  return 0;
}

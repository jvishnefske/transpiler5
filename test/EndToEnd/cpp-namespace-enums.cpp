// REQUIRES: cargo
// The RUNTIME oracle for the enum half of FR-108's namespace prefix.
//
// `cargo build` succeeding is compile-only evidence and cannot see a
// miscompile, and the failure this fix closes is exactly the kind a
// compile-clean crate hides: before the prefix, two same-shape enums in
// different namespaces MERGED into one Rust type, so every use site of the
// second resolved to the first's constants. Shapes that differ merely got
// refused loudly; shapes that agree were silently unified. This file diffs
// the built crate's stdout against the clang++-built native so both halves
// are observable.
//
// Every value derives from argc, so constant folding cannot pre-compute the
// answers and hide a miscompile behind a crate that builds. The pairs are
// chosen so a merge would be VISIBLE: `ns::Color::Red` (10) beside
// `::Color::Blue` (1) differ, and `p::E::A` (6) beside `q::E::A` (7)
// differ under an identical enumerator spelling -- the shape a bare-name
// key would have refused outright and a shape-keyed merge would have
// silently unified.
//
// The prefix ladder is exercised end to end: global vs `ns::`, a NESTED
// namespace, an anonymous namespace, and an enum inside `extern "C" { }`
// nested in a namespace (a LinkageSpecDecl is transparent and must
// contribute nothing beyond the enclosing namespace's prefix). Byte-
// identical vs `clang++ -std=c++17`.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_namespace_enums > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

enum Color { Blue = 1, Black = 2 };

namespace ns {
enum Color { Red = 10, Green = 20 };
} // namespace ns

namespace a {
namespace b {
enum Depth { Two = 3, Four = 6 };
} // namespace b
} // namespace a

namespace {
enum Hidden { Shy = 4 };
} // namespace

namespace cfgns {
extern "C" {
enum Cfg { CfgOn = 5 };
}
} // namespace cfgns

namespace p {
enum E { A = 6 };
} // namespace p

namespace q {
enum E { A = 7 };
} // namespace q

static int widen(Color g, ns::Color n) { return (int)g * 100 + (int)n; }

int main(int argc, char **argv) {
  int seed = argc; /* 1 at run time, opaque to the folder */
  Color g = Black;
  if (seed > 0)
    g = Blue;
  ns::Color n = ns::Green;
  if (seed > 0)
    n = ns::Red;
  a::b::Depth d = a::b::Two;
  if (seed > 1)
    d = a::b::Four;
  Hidden h = Shy;
  cfgns::Cfg c = cfgns::CfgOn;
  p::E pe = p::A;
  q::E qe = q::A;
  printf("%d %d %d %d %d %d %d\n", (int)g * seed, (int)n * seed, (int)d * seed,
         (int)h * seed, (int)c * seed, (int)pe * seed, (int)qe * seed);
  printf("%d\n", widen(g, n) + seed);
  // A merge of `p::E` into `q::E` (or the reverse) would make this compare
  // equal; two distinct types with distinct constants makes it 6 != 7.
  printf("%d\n", ((int)pe == (int)qe ? 0 : 1) * seed);
  return 0;
}

// REQUIRES: cargo
// FR-108 (namespace half): record names carry the same `ns_<name>_`
// prefix `cFunctionSymbolName` has always applied, so two records that
// share a C++ spelling in DIFFERENT namespaces COEXIST as two Rust types
// with two method sets. Before this wave `::Box` and `ns::Box` both
// emitted `Box`, the second definition was silently merged into the
// first, and every `ns::Box` call site dispatched to `::Box`'s body
// (measured: native `1 101`, emitted crate `1 1` — a silent miscompile
// with no templates involved).
//
// Pins the whole prefix ladder end to end: the global/`ns::` pair, a
// NESTED namespace (`a::b::Pt` -> `NsANsBPt`), an anonymous namespace
// (`ns_anon_`), and a record inside `extern "C" { }` nested in a
// namespace (a LinkageSpecDecl is transparent and contributes NOTHING,
// so it must keep the enclosing namespace's prefix and no more). Every
// value derives from argc, so constant folding cannot pre-compute the
// answers and hide a miscompile behind a compile-clean crate.
// Byte-identical vs `clang++ -std=c++17`.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_namespace_records > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct Box {
  int v;
  int get() const { return v; }
  void bump(int d) { v = v + d; }
};

namespace ns {
struct Box {
  int v;
  int get() const { return v + 100; }
  void bump(int d) { v = v + 2 * d; }
};
} // namespace ns

namespace a {
namespace b {
struct Pt {
  int x;
  int get() const { return x + 5; }
};
} // namespace b
} // namespace a

namespace {
struct Tally {
  int n;
  int get() const { return n * 3; }
};
} // namespace

namespace cfgns {
extern "C" {
struct Cfg {
  int v;
};
}
} // namespace cfgns

static int sum_boxes(Box g, ns::Box n) { return g.get() + n.get(); }

int main(int argc, char **argv) {
  int seed = argc; /* 1 at run time, opaque to the folder */
  Box g;
  g.v = seed;
  ns::Box n;
  n.v = seed;
  g.bump(seed + 1);
  n.bump(seed + 1);
  a::b::Pt p;
  p.x = seed * 7;
  Tally t;
  t.n = seed + 4;
  cfgns::Cfg c;
  c.v = seed * 11;
  printf("%d %d %d %d %d\n", g.get(), n.get(), p.get(), t.get(), c.v);
  printf("%d\n", sum_boxes(g, n));
  return 0;
}

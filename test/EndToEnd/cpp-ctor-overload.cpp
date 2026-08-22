// REQUIRES: cargo
// FR-114: the same-arity overloaded-CONSTRUCTOR differential, end to
// end — the motivating shape under raytracing's `interval` class, whose
// `interval(double,double)` / `interval(const interval&,const interval&)`
// pair both coded `_xx` before FR-114 and collided on one symbol. The
// widened member fallback codes them `S_new_dd` / `S_new_rsrs` (the new
// `r` prefix over the referenced type's code), so BOTH constructors now
// import; this leg builds and runs the `dd` one against the native.
// The `const S&` ctor is DEFINED but not CALLED at runtime: a struct
// lvalue passed to a const-T& constructor parameter is a pre-existing
// borrow-insertion gap proved independent of naming by the FR-114 spike
// (separate increment); the emitted crate carries the unused
// `s_new_rsrs` under its #![allow(dead_code)], and the Import-level test
// (test/Import/Cpp/overload-suffix.cpp) pins its symbol and signature.
// Every value derives from argc so constant folding cannot hide a
// miscompile. Byte-identical vs `clang++ -std=c++17`.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_ctor_overload > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

class S {
public:
  double a;
  double b;
  S(double x, double y) : a(x), b(y) {}
  S(const S &p, const S &q) : a(p.a), b(q.b) {}
  double span() const { return b - a; }
};

int main(int argc, char **argv) {
  int seed = argc; /* 1 at run time, opaque to the folder */
  S t(seed * 1.5, seed * 2.5);
  printf("%g %g\n", t.a, t.b); /* 1.5 2.5 */
  printf("%g\n", t.span());    /* 1 */
  return 0;
}

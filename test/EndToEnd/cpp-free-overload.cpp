// REQUIRES: cargo
// FR-114: the free-function overload differential, end to end. Before
// FR-114 this exact shape — `g(int)` and `g(double)` in one TU — was
// rejected with a wording that blamed a phantom second translation unit,
// and under --incremental the spurious "call argument type mismatch" it
// cascaded into stubbed c_main itself. Now the pair emits `g_i32`/`g_d`
// (one W2.15-style `_<code>` per parameter, applied only to genuine
// overload sets) and each call site resolves to the intended overload.
// The zero-param `random_double()` / two-param `random_double(lo, hi)`
// pair pins raytracing's motivating shape: the zero-param overload keeps
// the BARE name (a zero-arg C++ signature is unique in its set by
// construction). Every value derives from argc, so constant folding
// cannot pre-compute the answers and hide a miscompile behind a
// compile-clean crate. Byte-identical vs `clang++ -std=c++17`.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_free_overload > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

int g(int x) { return x * 2; }
double g(double x) { return x * 2.0; }

double random_double() { return 0.5; }
double random_double(double lo, double hi) { return lo + (hi - lo) * 0.25; }

int main(int argc, char **argv) {
  int seed = argc; /* 1 at run time, opaque to the folder */
  int a = g(seed);              /* g_i32: 2 */
  double d = g(seed * 2.0);     /* g_d: 4 */
  printf("%d %g\n", a, d);
  double r0 = random_double();          /* 0.5 */
  double r2 = random_double(r0, d + seed); /* 0.5 + 4.5*0.25 = 1.625 */
  printf("%g %g\n", r0, r2);
  return 0;
}

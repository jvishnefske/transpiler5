// REQUIRES: cargo
// Stage-D regression (bonus lift, gap G18): a defaulted call argument resolves
// to its default value at the call site, so a call omitting the argument and a
// call supplying it both produce the right result. Byte-matches the
// clang++-native build across several defaults (a plain constant, an
// expression default, and a second-parameter default). main returns 0 and
// reports via printf.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_default_arg > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

int add(int a, int b = 10) {
  return a + b;
}

int scale(int x, int f = 2 + 1) {
  return x * f;
}

int clamp(int v, int lo = 0, int hi = 100) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

int main() {
  printf("%d %d\n", add(5), add(5, 2));
  printf("%d %d\n", scale(4), scale(4, 10));
  printf("%d %d %d\n", clamp(-7), clamp(250), clamp(42, 10, 50));
  return 0;
}

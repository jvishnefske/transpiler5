// REQUIRES: cargo
// Broad-capture regression: modern C++ surface syntax that lifts and was not
// previously pinned end to end -- `auto` and `decltype` type deduction (both
// resolve to the deduced type, here int), a `noexcept` function specifier (no
// effect on the lowered signature), and the `nullptr` keyword (a null pointer
// constant, exercised in a comparison and a ternary). Byte-matches the
// clang++-native build; main returns 0 and reports via printf.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_modern_syntax > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

int inc(int x) noexcept {
  return x + 1;
}

int main() {
  auto a = 5;
  decltype(a) b = a * 2;
  int local = 42;
  int *p = nullptr;
  int c1 = (p == nullptr) ? -1 : *p;
  p = &local;
  int c2 = (p == nullptr) ? -1 : *p;
  auto sum = a + b + inc(a);
  printf("%d %d %d %d %d\n", a, b, sum, c1, c2);
  return 0;
}

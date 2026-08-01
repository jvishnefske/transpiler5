// REQUIRES: cargo
// Stage-B regression: a non-static data member initializer (NSDMI,
// `struct D { int x = 5; };`) makes the class's default constructor
// non-trivial, so `D d;` is a real 0-argument default construction rather
// than a vacuous (stripped) one. That constructor is never imported as a
// function; the importer instead applies each member's in-class initializer
// to the place directly (an NSDMI's `CXXDefaultInitExpr` resolving through
// `emitRValue` to the initializer). Byte-matches the clang++-native build.
// Only NSDMI-initialized fields are read (a field without an NSDMI would be
// indeterminate in C++ and is deliberately not observed). main returns 0 and
// reports via printf.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_nsdmi > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct D {
  int x = 5;
  int y = -7;
  unsigned u = 42u;
  long w = 1000000000L;
};

int main() {
  D d;
  printf("%d %d %u %ld\n", d.x, d.y, d.u, d.w);
  d.x += 1;
  d.u += 8u;
  printf("%d %u\n", d.x, d.u);
  return 0;
}

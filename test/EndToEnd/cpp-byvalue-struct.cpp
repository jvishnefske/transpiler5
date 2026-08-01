// REQUIRES: cargo
// Stage-B regression: a by-value struct PARAMETER (`int sum(Point p)`) and a
// by-value struct RETURN (`Point make(...)`) both surface as a trivial
// copy/move `CXXConstructExpr` in value position, which the importer now
// lowers as a whole-struct value copy (unwrap to the single source operand
// and load it whole). The parameter case also pins pass-by-value COPY
// semantics: the callee's mutation of `p` must not be visible to the caller's
// `q`. Byte-matches the clang++-native build; main returns 0 and reports via
// printf.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_byvalue_struct > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct Point {
  int x;
  int y;
};

int sum(Point p) {
  p.x += 100; // mutates the local copy only
  return p.x + p.y;
}

Point make(int a, int b) {
  Point r;
  r.x = a;
  r.y = b;
  return r; // by-value return (move construct of a POD)
}

int main() {
  Point q;
  q.x = 3;
  q.y = 4;
  int s = sum(q);
  Point m = make(5, 9);
  // s == 107; q is unchanged (3,4); m == (5,9).
  printf("%d %d %d %d %d\n", s, q.x, q.y, m.x, m.y);
  return 0;
}

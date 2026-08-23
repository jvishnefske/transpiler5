// REQUIRES: cargo
// W2.25: MEMBER by-value flat-call operator overloads, end to end —
// `operator==` defined OUT OF LINE (the shape FR-117 used to reject at the
// def), `operator!=` whose body spells the receiver as `*this`, a
// zero-extra-arg unary `operator-`, `operator[]` by value, a genuine
// member-operator overload set (`operator()(int)` / `operator()(int,int)`),
// the EXPLICIT member spelling `x.operator==(y)`, and dispatch on both
// sides of a comparison. Every value derives from argc so constant folding
// cannot hide a wrong-body dispatch; stdout is byte-diffed against the
// clang++ native.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_operator_member > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native seed > %t.native2.out
// RUN: %t.crate/target/release/cpp_operator_member seed > %t.rust2.out
// RUN: diff %t.native2.out %t.rust2.out

extern "C" int printf(const char *, ...);

struct S {
  int a;
  bool operator==(const S &o) const;
  bool operator!=(const S &o) const { return !(*this == o); }
  int operator[](int i) const { return a + i; }
  int operator()(int i) const { return a * i; }
  int operator()(int i, int j) const { return a * i + j; }
  S operator-() const {
    S r;
    r.a = -a;
    return r;
  }
};
bool S::operator==(const S &o) const { return a == o.a; }

int main(int argc, char **) {
  S x;
  x.a = argc;
  S y;
  y.a = 2;
  int v1 = (x == y) ? 10 : 20;
  int v2 = (x != y) ? 1 : 2;
  int v3 = x[3];
  int v4 = y[argc];
  int v5 = x(argc + 1);
  int v6 = x(argc, 9);
  int v7 = x.operator==(x) ? 5 : 6;
  S n = -x;
  printf("%d %d %d %d %d %d %d %d\n", v1, v2, v3, v4, v5, v6, v7, n.a);
  return 0;
}

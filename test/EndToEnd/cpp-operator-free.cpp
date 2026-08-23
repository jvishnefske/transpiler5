// REQUIRES: cargo
// W2.25: FREE by-value operator overloads, end to end — the vec3 shape
// that FR-119 recorded as this wave's motivating blocker: THREE free
// `operator*` overloads (V*int, int*V, V*V) that all synthesize the base
// `op_mul` and are split only by FR-114's per-parameter suffixes, plus
// `operator+` and a CHAINED use (`u + v + (u * v)`) whose intermediate
// temporaries feed the outer call as borrowed reference arguments. Every
// value derives from argc, so no constant folding can pre-compute the
// answers; stdout is byte-diffed against the clang++ native.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_operator_free > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native seed > %t.native2.out
// RUN: %t.crate/target/release/cpp_operator_free seed > %t.rust2.out
// RUN: diff %t.native2.out %t.rust2.out

extern "C" int printf(const char *, ...);

struct V {
  int x;
  int y;
};
V operator+(const V &a, const V &b) {
  V r;
  r.x = a.x + b.x;
  r.y = a.y + b.y;
  return r;
}
V operator*(const V &a, int t) {
  V r;
  r.x = a.x * t;
  r.y = a.y * t;
  return r;
}
V operator*(int t, const V &a) {
  V r;
  r.x = t * a.x + 1;
  r.y = t * a.y + 1;
  return r;
}
V operator*(const V &a, const V &b) {
  V r;
  r.x = a.x * b.x;
  r.y = a.y * b.y;
  return r;
}
bool operator!(const V &a) { return a.x == 0 && a.y == 0; }

int main(int argc, char **) {
  V u;
  u.x = argc;
  u.y = 2;
  V v;
  v.x = 3;
  v.y = argc + 4;
  V c = u + v + (u * v);
  V d = (u * argc) + (argc * v);
  V z;
  z.x = argc - argc;
  z.y = 0;
  int nz = (!z) ? 7 : 8;
  printf("%d %d %d %d %d\n", c.x, c.y, d.x, d.y, nz);
  return 0;
}

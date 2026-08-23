// REQUIRES: cargo
// W2.25 x W2.23 COMPOUNDING: one class carrying BOTH an admitted user copy
// constructor (W2.23's landed subset — the re-rank found most of the old
// operator mass re-rooted to copy-move BEHIND the operators, so this pair
// is the claimed unlock) and admitted member operators. The copy ctor
// mutates its target (`a = o.a + 1`), so a missed or extra copy diverges
// observably, and the `q == q` self-comparison exercises the FR-48
// receiver-and-argument shared-borrow shape. Every value derives from
// argc; stdout is byte-diffed against the clang++ native.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_operator_copy_ctor > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native seed > %t.native2.out
// RUN: %t.crate/target/release/cpp_operator_copy_ctor seed > %t.rust2.out
// RUN: diff %t.native2.out %t.rust2.out

extern "C" int printf(const char *, ...);

struct P {
  int a;
  int b;
  P() {
    a = 0;
    b = 0;
  }
  P(const P &o) {
    a = o.a + 1;
    b = o.b;
  }
  bool operator==(const P &o) const { return a == o.a && b == o.b; }
  bool operator<(const P &o) const { return a < o.a; }
};

int main(int argc, char **) {
  P p;
  p.a = argc;
  p.b = 7;
  P q = p;
  int e1 = (p == q) ? 1 : 0;
  int e2 = (q == q) ? 3 : 4;
  int e3 = (p < q) ? 5 : 6;
  printf("%d %d %d %d %d\n", p.a, q.a, e1, e2, e3);
  return 0;
}

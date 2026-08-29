// REQUIRES: cargo
// FR-123: the HIDDEN-FRIEND operator differential, end to end.
//
// This is the correctness oracle for the FR. A friend operator defined
// inline in its class used to be SILENTLY OMITTED from the emitted crate —
// the item walk never reached it, so there was no diagnostic and no item —
// which means the only way to know the fix actually computes the right
// values is to byte-diff the emitted crate's stdout against the
// `clang++ -std=c++17` native. A compile-clean `cargo build` cannot see a
// miscompile here; `diff` can.
//
// Every value derives from argc (1 with no argument, 2 with one), so
// constant folding cannot pre-compute the answers and hide a wrong
// operator behind a crate that happens to build. Both argc values are run.
//
// The shape exercises what the FR admits: an arithmetic hidden friend
// (`operator+`), a hidden-friend OVERLOAD SET on one class (`operator*`
// over V*V and V*int, split by FR-114's per-parameter suffixes), a
// comparison hidden friend returning bool (`operator==`), and a chained
// use whose intermediate temporaries feed the next call as borrowed
// references. The two `operator==` calls compare DISTINCT objects on
// purpose: `u == u` would borrow one object twice and hits the importer's
// pre-existing aliasing fence, which is a fact about argument borrowing
// and not about friends.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_operator_friend > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native seed > %t.native2.out
// RUN: %t.crate/target/release/cpp_operator_friend seed > %t.rust2.out
// RUN: diff %t.native2.out %t.rust2.out

extern "C" int printf(const char *, ...);

struct V {
  int x;
  int y;
  friend V operator+(const V &a, const V &b) {
    V r;
    r.x = a.x + b.x;
    r.y = a.y + b.y;
    return r;
  }
  friend V operator*(const V &a, const V &b) {
    V r;
    r.x = a.x * b.x;
    r.y = a.y * b.y;
    return r;
  }
  friend V operator*(const V &a, int t) {
    V r;
    r.x = a.x * t + 1;
    r.y = a.y * t + 1;
    return r;
  }
  friend bool operator==(const V &a, const V &b) {
    return a.x == b.x && a.y == b.y;
  }
};

int main(int argc, char **) {
  V u;
  u.x = argc;
  u.y = argc + 2;
  V v;
  v.x = 3;
  v.y = argc + 4;
  V c = u + v + (u * v);
  V d = (u * argc) + v;
  int eq = (u == v) ? 11 : 12;
  int eq2 = (c == d) ? 13 : 14;
  printf("%d %d %d %d %d %d\n", c.x, c.y, d.x, d.y, eq, eq2);
  return 0;
}

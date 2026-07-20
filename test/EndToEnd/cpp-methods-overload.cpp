// REQUIRES: cargo
// W2.2: a focused overload differential, end to end. `Multi::describe` has
// THREE overloads dispatched at four different call sites by BOTH
// argument count (one `int` vs two `int`s) and argument type (`int` vs
// `bool`) — the two axes the per-class mangling scheme in
// test/Import/Cpp/methods.cpp must keep distinct within a single class.
// (The exact suffix code this wave's scheme picks for `bool` is not
// pinned by this test — only that it produces a symbol distinct from the
// `int` overloads', so all four call sites resolve to the intended
// overload.) Byte-identical vs `clang++ -std=c++17`.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_methods_overload > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

class Multi {
public:
  int tag;
  int describe(int x) const { return tag + x; }
  int describe(int x, int y) const { return tag + x + y; }
  int describe(bool flag) const { return flag ? tag : 0; }
};

int main(void) {
  Multi m;
  m.tag = 10;
  int r1 = m.describe(5);
  int r2 = m.describe(5, 7);
  int r3 = m.describe(true);
  int r4 = m.describe(false);
  printf("r1=%d r2=%d r3=%d r4=%d\n", r1, r2, r3, r4);
  return 0;
}

// REQUIRES: cargo
// FR-112 byte-diff leg: a class carrying an omitted OPERATOR and an
// omitted BODY-FAIL method, with `main` touching neither, must go from an
// EMPTY emitted crate (the whole class plus everything naming it rejected,
// pre-FR-112) to a full one in STRICT mode -- and the full one must be
// byte-identical to the `clang++ -std=c++17` native. `cargo build` success
// alone cannot see a miscompile, so the stdout diff is the oracle; every
// printed value derives from argc so no constant fold can pre-compute the
// answers on both sides and hide a wiring bug.
//
// The one diagnostic allowed is the containment WARNING for the body-fail
// method (the operator is omitted silently, like FR-117's conversion
// functions); it is pinned against stderr so a regression back to a hard
// class-level rejection -- or a silent swallow of the method's own
// diagnostic -- fails here rather than in the diff.
//
// The emitted struct still derives Clone/Copy/Default, which is sound
// precisely because the copy/move-constructor gate STAYS class-level
// (cpp-contained-member-invalid.cpp's copy-ctor section): a class whose
// copies this crate bitwise-Copies is guaranteed to have wanted exactly
// that.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build 2>%t.err
// RUN: FileCheck %s --check-prefix=DIAG --input-file=%t.err
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_contained_member > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct Vec2 {
  int x;
  int y;
  Vec2() : x(0), y(0) {}
  void set(int a, int b) { x = a; y = b; }
  int sum() const { return x + y; }
  // Omitted silently: non-identifier DeclarationName.
  bool operator==(const Vec2 &o) const { return x == o.x && y == o.y; }
  // Omitted loudly: the body fails on the pointer cast.
  int bad() const { return *(int *)(long)x; }
};

int main(int argc, char **argv) {
  Vec2 v;
  v.set(argc, argc * 2 + 1);
  printf("sum=%d\n", v.sum());
  Vec2 w;
  w.set(v.sum(), argc);
  printf("x=%d y=%d\n", w.x, w.y);
  return 0;
}

// DIAG: warning: unsupported pointer expression: CStyleCastExpr (omitted: method 'bad' of class 'Vec2')

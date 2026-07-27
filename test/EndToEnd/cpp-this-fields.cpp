// REQUIRES: cargo
// W4: explicit `this->` field access + a bool struct field, end to end.
// The existing method tests only touch fields implicitly (a bare `value`)
// and only exercise bool as a local/return, never as a field. This builds
// a Rust crate through the full import + conversion + Rust-emission +
// `cargo build --release` pipeline and diffs its stdout against a
// `clang++ -std=c++17` build of the identical source, byte for byte.
// Exercises, across two instances (one member-init-list parameterized, one
// default-then-mutated): writes through explicit `this->` (`widen`,
// `toggle`), reads through explicit `this->` in const methods (`span`,
// `mid`, `state`), and a `bool` struct field driven by `!` and a ternary.
// Every reported value is data-dependent (distinct seeds at each call
// site), so a wiring bug surfaces as a diff mismatch rather than hiding
// behind a repeated literal.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_this_fields > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

class Box {
public:
  Box() : lo(0), hi(0), flipped(false) {}
  Box(int a, int b) : lo(a), hi(b), flipped(false) {}
  void widen(int d) {
    this->lo = this->lo - d;
    this->hi = this->hi + d;
  }
  void toggle() { this->flipped = !this->flipped; }
  int span() const { return this->hi - this->lo; }
  int mid() const { return (this->lo + this->hi) / 2; }
  int state() const { return this->flipped ? 1 : 0; }

private:
  int lo;
  int hi;
  bool flipped;
};

int main(void) {
  Box b(3, 8);
  b.widen(2);
  b.toggle();
  Box d;
  d.widen(5);

  int s1 = b.span();
  int m1 = b.mid();
  int st1 = b.state();
  int s2 = d.span();
  int m2 = d.mid();
  int st2 = d.state();

  printf("s1=%d m1=%d st1=%d s2=%d m2=%d st2=%d\n", s1, m1, st1, s2, m2, st2);
  return 0;
}

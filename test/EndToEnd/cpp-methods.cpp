// REQUIRES: cargo
// W2.2: genuine C++ non-virtual class methods, end to end. The real
// oracle for the whole wave: builds a Rust crate through the full import
// + conversion + Rust-emission + `cargo build --release` pipeline and
// diffs its stdout against a `clang++ -std=c++17` build of the identical
// source, byte for byte. Exercises, across two classes and three
// instances: a mutating method (`inc`), two const methods overloaded by
// arity (`get()` / `get(int)`), a static method (`Counter::origin()`), a
// default constructor AND a parameterized constructor both using a
// member-initializer list (never a body assignment), and a second class
// (`Adder`) whose two `sum` overloads are dispatched by argument count at
// two different call sites. All values are data-dependent (no compile-
// time-foldable literal duplicated between the native and transpiled
// paths would hide a wiring bug), and everything is reported through
// printf so lit's diff covers the only externally observable behavior.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_methods > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

class Counter {
public:
  Counter() : value(0) {}
  Counter(int start) : value(start) {}
  void inc(int d) { value = value + d; }
  int get() const { return value; }
  int get(int offset) const { return value + offset; }
  static int origin() { return 0; }

private:
  int value;
};

class Adder {
public:
  Adder(int base) : base_(base) {}
  int sum(int a) const { return base_ + a; }
  int sum(int a, int b) const { return base_ + a + b; }

private:
  int base_;
};

int main(void) {
  Counter c(5);
  Counter c2;
  c.inc(3);
  c.inc(1);
  int a = c.get();
  int b = c.get(10);
  int o = Counter::origin();
  int c2v = c2.get();

  Adder ad(100);
  int s1 = ad.sum(1);
  int s2 = ad.sum(1, 2);

  printf("a=%d b=%d o=%d c2=%d s1=%d s2=%d\n", a, b, o, c2v, s1, s2);
  return 0;
}

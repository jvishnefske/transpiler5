// REQUIRES: cargo
// W4: cross-instance data threading + a 3-way overload, end to end. Where
// cpp-methods drives independent instances, this threads values *between*
// instances and through a static-into-ctor composition, then builds a Rust
// crate through the full import + conversion + Rust-emission +
// `cargo build --release` pipeline and diffs its stdout against a
// `clang++ -std=c++17` build of the identical source, byte for byte.
// Exercises: a static method feeding a ctor argument (`Acc a(Acc::seed())`),
// one instance's const accessor result feeding another instance's mutating
// method (`b.add(a.value())`), and `add` overloaded three ways by both
// arity and type with mixed void/int returns (`add(int)`, `add(int,int)`,
// `add(bool)`). Every reported value is data-dependent and distinct, so a
// mis-threaded argument surfaces as a diff mismatch.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_method_chains > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

class Acc {
public:
  Acc() : value_(0) {}
  Acc(int start) : value_(start) {}
  void add(int x) { value_ = value_ + x; }
  int add(int x, int y) {
    value_ = value_ + x + y;
    return value_;
  }
  void add(bool flag) { value_ = value_ + (flag ? 1 : 0); }
  int value() const { return value_; }
  static int seed() { return 42; }

private:
  int value_;
};

int main(void) {
  Acc a(Acc::seed()); // value_ = 42
  a.add(8);           // 50
  int r = a.add(3, 4); // 57, r = 57
  a.add(true);        // 58

  Acc b;              // 0
  b.add(a.value());   // 58
  b.add(true);        // 59
  int bv = b.value(); // 59

  int av = a.value(); // 58

  printf("av=%d r=%d bv=%d\n", av, r, bv);
  return 0;
}

// REQUIRES: cargo
// W2.5: C++17 if/switch init-statements and condition declarations, end to
// end. The importer desugars all three forms by hoisting the declaration
// into the enclosing block; this test byte-diffs the torture shapes the
// corpus (test/Cpp17Suite/Inputs/002xx) deliberately keeps simple: TWO
// same-named init variables in one enclosing block (C++ scopes each to its
// own `if`; the desugar relies on decl-keyed locals emitting a Rust `let`
// shadowing), an `if` with BOTH slots populated (init-statement followed
// by a condition declaration), and a switch-with-init whose init variable
// is read inside case bodies. Byte-identical vs `clang++ -std=c++17`.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_init_statements > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

int f(int v) { return v - 1; }

int both_slots(int v) {
  if (int base = v - 3; int r = f(base)) {
    return base + r;
  } else {
    return -base;
  }
}

int main() {
  int a = 0;
  if (int r = f(5); r > 2) {
    a = r;
  }
  if (int r = f(9); r > 2) {
    a += r * 10;
  }
  int s = 0;
  switch (int r = f(3); r) {
  case 2:
    s = 20 + r;
    break;
  default:
    s = 90 + r;
    break;
  }
  printf("%d %d %d %d\n", a, s, both_slots(4), both_slots(0));
  return 0;
}

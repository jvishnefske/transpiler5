// REQUIRES: cargo
// FR-22 / FR-23: differential end-to-end test for C switch statements
// (including a fallthrough arm and a multi-label arm) and named enums.
// The crate build lowers the switch through cf.switch -> scf.index_switch ->
// emitrust.switch and renders a Rust `match`; the named enum renders as a
// Rust `enum` with Clone/Copy/PartialEq/Default derives. Both binaries must
// produce byte-identical stdout. --release is load-bearing: debug Rust
// panics on integer overflow where C wraps; the values below stay small.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/switch_enum > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

enum Color { Red, Green = 5, Blue };

int classify(enum Color c, int bonus) {
  int score = 0;
  switch ((int)c) {
  case 0:
    score = score + 1;
    /* fallthrough */
  case 5:
    score = score + 10;
    break;
  case 6:
    score = score + 100;
    break;
  default:
    score = -1;
    break;
  }
  return score + bonus;
}

int pick(int selector) {
  switch (selector) {
  case 1:
  case 2:
    return 20;
  case 3:
    return 30;
  default:
    return -1;
  }
}

int main(void) {
  enum Color colors[3];
  colors[0] = Red;
  colors[1] = Green;
  colors[2] = Blue;
  for (int i = 0; i < 3; ++i) {
    enum Color c = colors[i];
    int s = classify(c, i);
    printf("i=%d score=%d raw=%d\n", i, s, (int)c);
    if (c == Green) {
      printf("green seen\n");
    }
    if (c != Red) {
      printf("not red\n");
    }
  }
  for (int sel = 0; sel < 5; ++sel) {
    printf("sel=%d pick=%d\n", sel, pick(sel));
  }
  return 0;
}

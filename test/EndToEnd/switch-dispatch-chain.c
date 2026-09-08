// REQUIRES: cargo
// FR-207 differential test for the ADMITTED side of the dispatch-path
// fall-through bound. FR-198 bounded the plain-body path; this file pins that
// the bound added on the `emitDispatchSwitch` path did not quietly narrow what
// that path still lowers, and -- the part `cargo build` cannot see -- that the
// duplicating structurization it hands to `lift-cf-to-scf` is still CORRECT
// right up to the limit.
//
// Three shapes, all of which route to the dispatch lowering because their
// labels are not top-level children of the switch body:
//   `nested_ladder`  a 32-section fall-through chain one compound down. This
//                    is exactly the bound's boundary (the limit is "> 32"),
//                    the shape whose 33rd section is refused, and the one
//                    whose blowup was measured: 14300 emitted lines / 1.11s
//                    here, 44236 / 9.72s at 48, 190635 / 192.70s at 80.
//   `duff_ladder`    Duff's device -- labels inside a do-while body, so the
//                    fall-through crosses a back edge -- swept over
//                    data-dependent trip counts and every residue entry.
//   `wide_break`     64 nested labels with NO chain (every section breaks).
//                    Twice the rejected label count and admitted, which is
//                    what makes the bound a chain bound rather than a size
//                    bound; it must still compute the right answers.
//
// Every scrutinee is derived from `argc`, above and below the label range, so
// nothing folds at compile time and a miscompile cannot hide behind a
// constant. Byte-identical stdout and exit codes against the clang-built
// native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/switch_dispatch_chain > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int nested_ladder(int x) {
  int acc = 0;
  switch (x) {
    if (x >= 0) {
      case 0: acc += 1;
      case 1: acc += 8;
      case 2: acc += 2;
      case 3: acc += 9;
      case 4: acc += 3;
      case 5: acc += 10;
      case 6: acc += 4;
      case 7: acc += 11;
      case 8: acc += 5;
      case 9: acc += 12;
      case 10: acc += 6;
      case 11: acc += 13;
      case 12: acc += 7;
      case 13: acc += 1;
      case 14: acc += 8;
      case 15: acc += 2;
      case 16: acc += 9;
      case 17: acc += 3;
      case 18: acc += 10;
      case 19: acc += 4;
      case 20: acc += 11;
      case 21: acc += 5;
      case 22: acc += 12;
      case 23: acc += 6;
      case 24: acc += 13;
      case 25: acc += 7;
      case 26: acc += 1;
      case 27: acc += 8;
      case 28: acc += 2;
      case 29: acc += 9;
      case 30: acc += 3;
      case 31: acc += 10;
    }
  }
  return acc;
}

int duff_ladder(int x, int n) {
  int acc = 0;
  switch (x) {
  case 0: do { acc += 1;
  case 1:      acc += 6;
  case 2:      acc += 11;
  case 3:      acc += 5;
  case 4:      acc += 10;
  case 5:      acc += 4;
  case 6:      acc += 9;
  case 7:      acc += 3;
  case 8:      acc += 8;
  case 9:      acc += 2;
  case 10:      acc += 7;
  case 11:      acc += 1;
  case 12:      acc += 6;
  case 13:      acc += 11;
  case 14:      acc += 5;
  case 15:      acc += 10;
             } while (--n > 0);
    break;
  default: acc = -7;
  }
  return acc;
}

int wide_break(int x) {
  int acc = 0;
  switch (x) {
    if (x >= 0) {
      case 0: acc += 1; break;
      case 1: acc += 4; break;
      case 2: acc += 7; break;
      case 3: acc += 10; break;
      case 4: acc += 13; break;
      case 5: acc += 16; break;
      case 6: acc += 2; break;
      case 7: acc += 5; break;
      case 8: acc += 8; break;
      case 9: acc += 11; break;
      case 10: acc += 14; break;
      case 11: acc += 17; break;
      case 12: acc += 3; break;
      case 13: acc += 6; break;
      case 14: acc += 9; break;
      case 15: acc += 12; break;
      case 16: acc += 15; break;
      case 17: acc += 1; break;
      case 18: acc += 4; break;
      case 19: acc += 7; break;
      case 20: acc += 10; break;
      case 21: acc += 13; break;
      case 22: acc += 16; break;
      case 23: acc += 2; break;
      case 24: acc += 5; break;
      case 25: acc += 8; break;
      case 26: acc += 11; break;
      case 27: acc += 14; break;
      case 28: acc += 17; break;
      case 29: acc += 3; break;
      case 30: acc += 6; break;
      case 31: acc += 9; break;
      case 32: acc += 12; break;
      case 33: acc += 15; break;
      case 34: acc += 1; break;
      case 35: acc += 4; break;
      case 36: acc += 7; break;
      case 37: acc += 10; break;
      case 38: acc += 13; break;
      case 39: acc += 16; break;
      case 40: acc += 2; break;
      case 41: acc += 5; break;
      case 42: acc += 8; break;
      case 43: acc += 11; break;
      case 44: acc += 14; break;
      case 45: acc += 17; break;
      case 46: acc += 3; break;
      case 47: acc += 6; break;
      case 48: acc += 9; break;
      case 49: acc += 12; break;
      case 50: acc += 15; break;
      case 51: acc += 1; break;
      case 52: acc += 4; break;
      case 53: acc += 7; break;
      case 54: acc += 10; break;
      case 55: acc += 13; break;
      case 56: acc += 16; break;
      case 57: acc += 2; break;
      case 58: acc += 5; break;
      case 59: acc += 8; break;
      case 60: acc += 11; break;
      case 61: acc += 14; break;
      case 62: acc += 17; break;
      case 63: acc += 3; break;
    }
  }
  return acc;
}

int main(int argc, char **argv) {
  int s = argc;
  int total = 0;
  for (int i = -3; i < 36; ++i) {
    int v = i * s;
    int a = nested_ladder(v);
    total += a;
    printf("nested %d %d\n", v, a);
  }
  for (int i = -3; i < 20; ++i) {
    int v = i * s;
    for (int t = 0; t < 4; ++t) {
      int b = duff_ladder(v, t * s + 1);
      total += b;
      printf("duff %d %d %d\n", v, t * s + 1, b);
    }
  }
  for (int i = -3; i < 68; ++i) {
    int v = i * s;
    int c = wide_break(v);
    total += c;
    printf("wide %d %d\n", v, c);
  }
  printf("total %d\n", total);
  return 0;
}

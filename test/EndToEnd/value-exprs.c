// REQUIRES: cargo
// Differential regression test for expression-position features: the
// conditional operator (with short-circuit arm evaluation), the comma
// operator, assignments/compound assignments/increment/decrement used as
// values, constant-folded sizeof/_Alignof, and do-while loops.
// Byte-identical stdout and exit codes against the clang-built native
// binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/value_exprs > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int ternary(int c, int a, int b) {
  // Only the selected arm's assignment runs.
  int x = 0;
  int y = 0;
  int r = c ? (x = a + 1) : (y = b - 1);
  return r * 100 + x * 10 + y;
}

int nested_ternary(int v) {
  return v < 0 ? -1 : (v == 0 ? 0 : (v > 100 ? 2 : 1));
}

double mixed_ternary(int c, int i, double d) {
  return c ? i : d;
}

int comma(int a) {
  int b;
  b = (a += 3, a * 2);
  // Comma in a for header.
  int s = 0;
  int j = 0;
  for (int i = 0; i < 3; i++, j += 2) {
    s += i + j;
  }
  return b + s + j;
}

int value_positions(int x) {
  int y = (x = 5);
  int z = x++;
  int w = --x;
  int v = (x *= 3);
  int chain = (y = (z = w + 1) + 2);
  return y * 10000 + z * 1000 + w * 100 + v * 10 + x + chain;
}

int sizes(void) {
  int a[7];
  a[0] = 1;
  return (int)(sizeof(char) + 10 * sizeof(short) + 100 * sizeof(int) +
               1000 * sizeof(long) + (int)sizeof a + (int)_Alignof(double) +
               (int)sizeof a[0]);
}

int do_while(int n) {
  int s = 0;
  do {
    s += n;
    n--;
    if (n == 2) {
      continue;
    }
    if (s > 40) {
      break;
    }
  } while (n > 0);
  // A do-while that runs exactly once.
  int once = 0;
  do {
    once++;
  } while (0);
  return s * 10 + once;
}

int main(void) {
  for (int c = 0; c < 2; c++) {
    printf("ternary(%d)=%d\n", c, ternary(c, 4, 9));
  }
  int probes[5];
  probes[0] = -5;
  probes[1] = 0;
  probes[2] = 1;
  probes[3] = 100;
  probes[4] = 101;
  for (int i = 0; i < 5; i++) {
    printf("nested(%d)=%d\n", probes[i], nested_ternary(probes[i]));
  }
  printf("mixed0=%f mixed1=%f\n", mixed_ternary(0, 3, 2.5),
         mixed_ternary(1, 3, 2.5));
  printf("comma=%d\n", comma(2));
  printf("values=%d\n", value_positions(7));
  printf("sizes=%d\n", sizes());
  for (int n = 1; n < 12; n += 5) {
    printf("dw(%d)=%d\n", n, do_while(n));
  }
  int cond_in_loop = 0;
  int i = 0;
  do {
    cond_in_loop += i % 2 ? 3 : 1;
  } while ((i += 1, i) < 6);
  printf("mixed_loop=%d\n", cond_in_loop);
  printf("exit=%d\n", nested_ternary(42));
  return 0;
}

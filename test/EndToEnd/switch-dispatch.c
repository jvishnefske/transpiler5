// REQUIRES: cargo
// Differential test for the dispatch switch lowering (CTS-S2): switch
// bodies that are not plain compound statements and case labels nested
// inside inner statements. Covers Duff's device (jump into a do-while
// body) swept over data-dependent trip counts and every residue entry
// point, case labels inside both arms of an if, a non-compound switch
// body, a statement before the first label (dead prefix), negative and
// INT_MIN/INT_MAX boundary case values swept over all cases plus no-match
// values on both sides (FR-25), and break/fall-through interleaving under
// the dispatch. Byte-identical stdout and exit codes against the
// clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/switch_dispatch > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int duff_copy(int count) {
  int from[24];
  int to[24];
  int sum = 0;
  int n, i;
  for (i = 0; i < 24; i++) {
    from[i] = i * 3 + 1;
    to[i] = 0;
  }
  i = 0;
  n = (count + 7) / 8;
  switch (count % 8) {
  case 0: do { to[i] = from[i]; i = i + 1;
  case 7:      to[i] = from[i]; i = i + 1;
  case 6:      to[i] = from[i]; i = i + 1;
  case 5:      to[i] = from[i]; i = i + 1;
  case 4:      to[i] = from[i]; i = i + 1;
  case 3:      to[i] = from[i]; i = i + 1;
  case 2:      to[i] = from[i]; i = i + 1;
  case 1:      to[i] = from[i]; i = i + 1;
          } while (--n > 0);
  }
  for (i = 0; i < 24; i++)
    sum = sum + to[i];
  return sum;
}

int case_in_if(int x, int f) {
  int r = 0;
  switch (x) {
  case 0:
    if (f) {
  case -1:
      r = r + 5;
    } else {
  case 2:
      r = r + 6;
    }
    r = r + 100;
    break;
  case (-2147483647 - 1):
    r = 1000;
    break;
  case 2147483647:
    r = 2000;
    break;
  default:
    r = 9999;
  }
  return r;
}

int noncompound(int x) {
  switch (x)
    case 3:
      return 7;
  switch (x)
    default:
      return x + 40;
}

int dead_prefix(int x) {
  switch (x) {
    {
      x = 55;
  case 1:
      return x + 500;
    }
  case 2:
    return 600;
  }
  return x;
}

int case_in_loop_break(int x) {
  int r = 0;
  int k = 0; /* entering the loop body at case 5 skips the for's init */
  switch (x) {
  case 0:
    for (k = 0; k < 10; k++) {
      if (k == 4)
        break; /* binds to the for, not the switch */
  case 5:
      r = r + 1;
    }
    r = r + 20;
    break;
  default:
    r = 300;
  }
  return r;
}

int main(void) {
  int c, f;
  /* Duff's device: every entry residue and short data-dependent trip
     counts, including a full multiple of 8. */
  for (c = 1; c <= 24; c++)
    printf("duff %d -> %d\n", c, duff_copy(c));
  /* All cases plus a no-match value on both sides, both if arms. */
  for (f = 0; f <= 1; f++) {
    printf("if %d %d -> %d\n", 0, f, case_in_if(0, f));
    printf("if %d %d -> %d\n", -1, f, case_in_if(-1, f));
    printf("if %d %d -> %d\n", 2, f, case_in_if(2, f));
    printf("if %d %d -> %d\n", -2147483647 - 1, f,
           case_in_if(-2147483647 - 1, f));
    printf("if %d %d -> %d\n", 2147483647, f, case_in_if(2147483647, f));
    printf("if %d %d -> %d\n", 3, f, case_in_if(3, f));
    printf("if %d %d -> %d\n", -7, f, case_in_if(-7, f));
  }
  for (c = -1; c <= 4; c++)
    printf("nc %d -> %d\n", c, noncompound(c));
  for (c = 0; c <= 3; c++)
    printf("dp %d -> %d\n", c, dead_prefix(c));
  printf("lb %d -> %d\n", 0, case_in_loop_break(0));
  printf("lb %d -> %d\n", 5, case_in_loop_break(5));
  printf("lb %d -> %d\n", 6, case_in_loop_break(6));
  return 0;
}

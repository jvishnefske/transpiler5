// REQUIRES: cargo
// Differential regression test for the generality fixes beyond the original
// switch/enum test vectors: negative and sparse case labels (previously the
// sign-extending scrutinee cast made them silently take the default arm),
// INT_MIN and large-magnitude negative labels (incl. a negative 64-bit
// label below INT32_MIN, pinning the in-memory zero-extension path),
// 64-bit switch with negative case labels, nested switch, enums with
// negative discriminants, float != (arith.cmpf une), and early-return
// chains that canonicalize to arith.select. Byte-identical stdout and exit
// codes against the clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/switch_general > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

enum Temp { Cold = -5, Mild, Hot = 100 };

int sparse(int v) {
  switch (v) {
  case (-2147483647 - 1):
    return 7;
  case -2147483647:
    return 1;
  case -7:
    return 2;
  case -1:
    return 3;
  case 0:
    return 4;
  case 42:
    return 5;
  case 2147483647:
    return 6;
  default:
    return 0;
  }
}

int wide(long v) {
  switch (v) {
  case -1:
    return 1;
  case 4294967296:
    return 2;
  case -4294967296:
    return 3;
  default:
    return 0;
  }
}

int nested(int a, int b) {
  switch (a) {
  case 1:
    switch (b) {
    case -3:
      return 13;
    case 4:
      return 14;
    default:
      return 10;
    }
  case -2:
    return 2;
  default:
    switch (b) {
    case 1:
      return 91;
    default:
      return 90;
    }
  }
}

int early(int c) {
  if (c == 1) {
    return 5;
  }
  if (c == 2) {
    return 7;
  }
  return 9;
}

int describe(enum Temp t) {
  switch ((int)t) {
  case -5:
    return 1;
  case -4:
    return 2;
  case 100:
    return 3;
  default:
    return 0;
  }
}

int main(void) {
  int vals[9];
  vals[0] = -2147483647;
  vals[1] = -7;
  vals[2] = -1;
  vals[3] = 0;
  vals[4] = 42;
  vals[5] = 2147483647;
  vals[6] = 5;
  vals[7] = -100;
  vals[8] = -2147483647 - 1;
  for (int i = 0; i < 9; ++i) {
    printf("sparse(%d)=%d\n", vals[i], sparse(vals[i]));
  }
  long wvals[4];
  wvals[0] = -1;
  wvals[1] = 4294967296;
  wvals[2] = 0;
  wvals[3] = -4294967296;
  for (int i = 0; i < 4; ++i) {
    printf("wide(%ld)=%d\n", wvals[i], wide(wvals[i]));
  }
  for (int a = -2; a < 3; ++a) {
    for (int b = -3; b < 5; ++b) {
      printf("nested(%d,%d)=%d\n", a, b, nested(a, b));
    }
  }
  for (int c = 0; c < 4; ++c) {
    printf("early(%d)=%d\n", c, early(c));
  }
  enum Temp temps[3];
  temps[0] = Cold;
  temps[1] = Mild;
  temps[2] = Hot;
  for (int i = 0; i < 3; ++i) {
    enum Temp t = temps[i];
    printf("describe=%d cold=%d\n", describe(t), t == Cold);
  }
  double x = 1.5;
  double y = 2.5;
  if (x != y) {
    printf("floats differ\n");
  }
  if (x != x) {
    printf("never\n");
  }
  return 0;
}

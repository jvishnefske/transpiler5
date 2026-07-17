// REQUIRES: cargo
// CTS-S6: differential end-to-end test for integer-to-enum conversion, the
// reverse of C99-5's enum-to-int direction. C enum objects hold any value
// of the underlying type (C99 6.7.2.2), so the transpiled open enum must
// preserve data-dependent values covering every declared enumerator AND
// values matching no declared enumerator (12 and a negative), and convert
// back to int unchanged. The address of an enum object read through a
// pointer to its underlying type must observe the stored raw value. Both
// binaries must produce byte-identical stdout.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/enum_from_int > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

enum Fred { A, B, C, D, E = 54, F = 73, G, H };
enum Temp { Cold = -3, Warm = 1 };

static unsigned int deref_uint(unsigned int *p) {
  return *p;
}

int main(void) {
  /* Every declared enumerator's value, plus undeclared 12 and 200, all
     data-dependent (read from an array the optimizer cannot fold). */
  int values[10];
  values[0] = 0;
  values[1] = 1;
  values[2] = 2;
  values[3] = 3;
  values[4] = 54;
  values[5] = 73;
  values[6] = 74;
  values[7] = 75;
  values[8] = 12;
  values[9] = 200;
  for (int i = 0; i < 10; ++i) {
    enum Fred f;
    f = values[i];
    printf("f=%d\n", (int)f);
  }

  /* Signed-underlying enum: undeclared negative values are preserved. */
  for (int i = 0; i < 3; ++i) {
    enum Temp t;
    t = i - 4;
    printf("t=%d\n", (int)t);
    if (t == Cold) {
      printf("cold\n");
    }
  }

  /* Enumerator assignments keep the direct constant path. */
  enum Fred e = E;
  printf("e=%d\n", (int)e);

  /* The enum's address read through a pointer to its underlying type. */
  enum Fred g;
  g = values[8];
  printf("raw=%u\n", deref_uint(&g));
  return 0;
}

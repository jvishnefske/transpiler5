// REQUIRES: cargo
// FR-23 / C99-5: differential end-to-end test for mixed enum/int
// comparisons and conversions. Color has no negative enumerator, so C
// gives it an unsigned underlying type: comparing against a negative int
// converts that int to a huge unsigned value, and the transpiled Rust must
// reproduce exactly that (the enum renders `as u32` next to the converted
// literal). Temp's negative enumerator forces a signed underlying type and
// signed comparisons. Both binaries must produce byte-identical stdout.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/enum_int > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

enum Color { Red, Green = 5, Blue };
enum Temp { Cold = -3, Mild = 1, Hot };

int main(void) {
  enum Color c = Green;
  enum Temp t = Cold;

  /* Unsigned underlying type: -1 converts to UINT_MAX, so the relational
     answers differ from naive signed comparison. */
  printf("c>-1 %d\n", c > -1);
  printf("c==-1 %d\n", c == -1);
  printf("-1<c %d\n", -1 < c);
  printf("c<0 %d\n", c < 0);
  printf("c==5 %d\n", c == 5);
  printf("5==c %d\n", 5 == c);
  printf("c!=4 %d\n", c != 4);

  /* Signed underlying type: negative values compare arithmetically. */
  printf("t<-1 %d\n", t < -1);
  printf("t==-3 %d\n", t == -3);
  printf("-4<t %d\n", -4 < t);
  printf("t>0 %d\n", t > 0);

  /* Enumerator constants (type int in C) against int literals, both
     orders, like c-testsuite 00054/00055. */
  printf("Red!=0 %d\n", Red != 0);
  printf("0==Red %d\n", 0 == Red);
  printf("Green>=5 %d\n", Green >= 5);
  printf("Cold==-3 %d\n", Cold == -3);

  /* Explicit casts and enum values feeding integer arithmetic. */
  printf("int-c %d\n", (int)c);
  printf("uns-c %d\n", (int)((unsigned)c + 1u));
  printf("sum %d\n", (int)(c + t));
  printf("scaled %d\n", (int)c * 2 - t);

  /* Enum truthiness in an if condition. */
  if (c) {
    printf("c truthy\n");
  }
  t = Mild;
  printf("t==1 %d\n", t == 1);
  return 0;
}

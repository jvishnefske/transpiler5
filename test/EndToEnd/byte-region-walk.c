// REQUIRES: cargo
// CTS-BR (00216), differential end-to-end test: byte-exact walks of
// u8-only byte-region aggregates against the natively compiled C
// program. Walked objects: a string-initialized struct global, a
// flexible-array-member struct (its sizeof-bounded 22 fixed bytes only,
// with the FAM tail initialized but never read), an unnamed-arm union
// (whole region, single member, copy, and a write through the NAMED
// arm), plus locals with data-dependent contents written at runtime
// through members before walking. All values printed as %x bytes are
// C-guaranteed: explicit initializers, C99 zero fill, or runtime
// stores — never padding (every type is align-1 and padding-free), so
// the byte streams are deterministic in both languages. The program has
// no UB; main returns 0.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/byte_region_walk > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>

typedef unsigned char u8;

struct S { u8 a, b; u8 c[2]; };
struct T { u8 s[16]; u8 a; };
struct V { struct S s; struct T t; u8 a; };
struct W {
  struct V t;
  struct S s[];
};
union UV {
  struct { u8 a, b; };
  struct S s;
};

struct W gw = {{1, 2, 3, 4}, {1, 2, 3, 4, 5}};
union UV guv = {{6, 5}};
struct T gt = {"hello", 42};

void print_(const char *name, const u8 *p, long size) {
  printf("%s:", name);
  while (size--) {
    printf(" %x", *p++);
  }
  printf("\n");
}
#define print(x) print_(#x, (u8 *)&x, sizeof(x))

void walk_w(struct W *w) {
  print_("w", (u8 *)w, sizeof(struct W));
  struct S first = w->t.s;
  print(first);
}

int main(void) {
  print(gt);
  print(gw); /* the FAM tail is outside sizeof: 22 bytes walked */
  print(guv);
  print(guv.b);
  walk_w(&gw);

  /* Data-dependent contents: runtime writes through members. */
  struct T lt = {"hello", 42};
  int i;
  for (i = 0; i < 16; i++) {
    lt.s[i] = (u8)(i * 3 + 1);
  }
  lt.a = lt.s[5];
  print(lt);

  union UV luv = guv;   /* whole-region union copy */
  luv.s.c[1] = lt.s[2]; /* write through the named arm, offset 3 */
  print(luv);

  struct S ls = {1, 2, {3, 4}};
  const struct S *pls = &ls;
  struct S ls2 = *pls; /* init from a deref */
  ls2.b = (u8)(ls2.b + lt.a);
  print(ls2);

  return 0;
}

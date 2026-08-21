// REQUIRES: cargo
// FR-107 pointer-ARRAY struct members, differential end-to-end test.
// `cargo build` success is compile-only and CANNOT see a miscompile, so
// the oracle here is a BYTE-DIFF of the emitted crate's stdout against
// the clang-built native, in FOUR argc branches: every value that flows
// derives from argc, so constant folding cannot hide a wrong field
// offset or a shifted slot run behind an identical constant.
//
// What is under test is that the `[i64; N]` slot run is INERT and
// correctly SIZED. A pointer-array member is admitted as a type only —
// no element is ever bound to a target object and every element use is a
// located rejection (pointers-member-array-invalid.c) — so this program
// never touches the slots. What it must prove is that admitting them
// does not disturb anything around them: the record's OTHER members must
// keep their values through every instance form, in the emitted crate
// exactly as in C. A slot run emitted at the wrong width, or an
// initializer that stored into the wrong field, would show up here as a
// diverging field value and nowhere else.
//
// The record is deliberately the lwIP `struct netif` shape: a scalar
// before the runs, three runs of different element types and lengths
// (`void *`, `struct T *`, `const char *`), and both a scalar and a byte
// array after them, so a mis-sized run shifts an observable member.
//
// Every instance form the admission has to survive is exercised: a
// global with a designated initializer, a `= {0}` local, a local with
// EXPLICIT all-null braces for each run (the FR-107 initializer branch),
// an array of records, a by-pointer parameter mutated in place, a
// by-value return, and a whole-record copy.
//
// main returns 0 and reports everything on stdout, so lit's exit-code
// checking covers all four runs and diff covers the observable behavior.
// The program has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native0.out
// RUN: %t.crate/target/release/pointers_member_array > %t.rust0.out
// RUN: diff %t.native0.out %t.rust0.out
// RUN: %t.native one > %t.native1.out
// RUN: %t.crate/target/release/pointers_member_array one > %t.rust1.out
// RUN: diff %t.native1.out %t.rust1.out
// RUN: %t.native one two three > %t.native3.out
// RUN: %t.crate/target/release/pointers_member_array one two three > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out
// RUN: %t.native a b c d e > %t.native5.out
// RUN: %t.crate/target/release/pointers_member_array a b c d e > %t.rust5.out
// RUN: diff %t.native5.out %t.rust5.out

#include <stdio.h>

struct T { int tag; };

struct netifish {
  int index;
  void *client_data[4];
  struct T *slots[3];
  const char *names[2];
  int mtu;
  unsigned char hwaddr[6];
};

struct nested {
  struct netifish inner;
  int cookie;
};

static struct netifish gdesig = { .index = 9, .mtu = 1400 };

static void fill(struct netifish *n, int seed) {
  n->index = seed * 3 - 1;
  n->mtu = 1500 - seed * 7;
  for (int i = 0; i < 6; i++)
    n->hwaddr[i] = (unsigned char)(seed * 11 + i * 5);
}

static int bump(struct netifish *n, int by) {
  n->mtu += by;
  n->index -= by;
  return n->mtu + n->index;
}

static struct netifish make(int seed) {
  struct netifish h = {0};
  fill(&h, seed);
  h.mtu += seed;
  return h;
}

static void report(const char *tag, const struct netifish *n) {
  printf("%s idx=%d mtu=%d hw=%u,%u,%u\n", tag, n->index, n->mtu,
         n->hwaddr[0], n->hwaddr[3], n->hwaddr[5]);
}

int main(int argc, char **argv) {
  int seed = argc;

  printf("gdesig idx=%d mtu=%d\n", gdesig.index, gdesig.mtu);
  gdesig.mtu += seed * 4;
  gdesig.index += seed;
  struct netifish gsnap = gdesig;
  report("gdesig2", &gsnap);

  struct netifish zero = {0};
  fill(&zero, seed + 1);
  report("zero", &zero);

  /* EXPLICIT all-null braces for each pointer-array run: the shape real
     declarations write, and the one the FR-107 initializer branch
     accepts. */
  struct netifish braced = { 5, {0}, {0}, {0}, 900, {1, 2, 3, 4, 5, 6} };
  printf("braced idx=%d mtu=%d hw0=%u hw5=%u\n", braced.index, braced.mtu,
         braced.hwaddr[0], braced.hwaddr[5]);
  printf("bump=%d\n", bump(&braced, seed * 13));
  report("braced2", &braced);

  struct netifish table[3] = {
      {1, {0}, {0}, {0}, 100, {0}},
      {2, {0}, {0}, {0}, 200, {0}},
      {3, {0}, {0}, {0}, 300, {0}},
  };
  for (int i = 0; i < 3; i++) {
    fill(&table[i], seed + i * 2);
    table[i].mtu += i;
    report("t", &table[i]);
  }

  struct netifish ret = make(seed * 2);
  report("ret", &ret);

  struct netifish copy = ret;
  copy.mtu += 17;
  copy.index *= 2;
  report("copy", &copy);
  report("ret-after-copy", &ret);

  struct nested nest = {0};
  fill(&nest.inner, seed + 4);
  nest.cookie = seed * 31;
  report("nest", &nest.inner);
  printf("cookie=%d\n", nest.cookie);

  int total = gsnap.mtu + zero.index + braced.mtu + table[2].index +
              ret.mtu + copy.index + nest.cookie;
  printf("total=%d\n", total);
  return 0;
}

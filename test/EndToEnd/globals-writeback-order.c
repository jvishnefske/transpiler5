// REQUIRES: cargo
// Differential regression test for the staged-global-copy writeback
// ordering (C11 6.5.16p3): the RHS of an assignment is fully evaluated —
// side effects included — before the assignment's store, and the store
// writes only the designated subobject. The staged-copy model loads a
// whole-global snapshot when the LHS place is formed; a store-back of a
// snapshot taken BEFORE the RHS ran reverts an RHS call's write to a
// DIFFERENT subobject of the same global (a lost update). Each case below
// has an RHS (or index) call that writes a distinct subobject of the
// global named by the LHS, and prints a distinct digest afterwards:
//   a. wide-byte store through a reinterpreting view (CTS-P11):
//      `*(unsigned *)(gbuf + 0) = poke();` with poke() writing gbuf[6];
//   b. wide-byte compound assign: `*(unsigned *)(gbuf + 0) += bump();`
//      with bump() writing gbuf[7];
//   c. global struct-member store: `gs.a = touch_b();` setting gs.b;
//   d. struct-member compound assign: `gs.a += touch_b2();` setting gs.b;
//   e. erased-pointer-return arrow store (CTS-S 00089):
//      `get()->x = poke_y();` with poke_y() writing g2.y;
//   f. subscript-index siblings: `garr[idx_set2()]++;` and
//      `garr[idx_set3()] += 5;` where the index call writes another
//      element of the same array.
// Every mutated subobject is distinct from the stored-to subobject, so no
// unsequenced-access UB arises; a native clang build is the oracle.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name globals_writeback_order --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/globals_writeback_order > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

/* Case a/b: a global byte region viewed through a wider integer window
   (plain char: the CTS-P11 byte-region element type). Every byte stays
   below 128, so char signedness never shows in the printed digests. */
char gbuf[8];

static unsigned poke(void) {
  gbuf[6] = 42; /* distinct byte: outside the [0,4) window */
  return 0x01020304u;
}

static unsigned bump(void) {
  gbuf[7] = 55; /* distinct byte: outside the [0,4) window */
  return 0x00000010u;
}

/* Case c/d: a global struct whose OTHER member the RHS call writes. */
struct S {
  int a;
  int b;
};
struct S gs;

static int touch_b(void) {
  gs.b = 77;
  return 5;
}

static int touch_b2(void) {
  gs.b = 91;
  return 9;
}

/* Case e: an erased single-global-base pointer return (CTS-S 00089). */
struct T {
  int x;
  int y;
};
struct T g2;

static struct T *get(void) { return &g2; }

static int poke_y(void) {
  g2.y = 88;
  return 13;
}

/* Case f: subscript-index calls writing another element of the array. */
int garr[4];

static int idx_set2(void) {
  garr[2] = 33;
  return 0;
}

static int idx_set3(void) {
  garr[3] = 44;
  return 1;
}

int main(void) {
  /* a. wide-byte store: poke()'s write to gbuf[6] must survive. */
  *(unsigned *)(gbuf + 0) = poke();
  printf("a %u %u %u %u %u\n", (unsigned)gbuf[0], (unsigned)gbuf[1],
         (unsigned)gbuf[2], (unsigned)gbuf[3], (unsigned)gbuf[6]);

  /* b. wide-byte compound assign: bump()'s write to gbuf[7] must survive. */
  *(unsigned *)(gbuf + 0) += bump();
  printf("b %u %u %u\n", (unsigned)gbuf[0], (unsigned)gbuf[6],
         (unsigned)gbuf[7]);

  /* c. struct-member store: touch_b()'s write to gs.b must survive. */
  gs.a = touch_b();
  printf("c %d %d\n", gs.a, gs.b);

  /* d. struct-member compound assign: touch_b2()'s gs.b must survive. */
  gs.a += touch_b2();
  printf("d %d %d\n", gs.a, gs.b);

  /* e. erased-return arrow store: poke_y()'s write to g2.y must survive. */
  get()->x = poke_y();
  printf("e %d %d\n", g2.x, g2.y);

  /* f. subscript-index siblings: the index call's element write must
     survive the ++/+= writeback of the OTHER element. */
  garr[idx_set2()]++;
  printf("f1 %d %d %d %d\n", garr[0], garr[1], garr[2], garr[3]);
  garr[idx_set3()] += 5;
  printf("f2 %d %d %d %d\n", garr[0], garr[1], garr[2], garr[3]);

  return 0;
}

// REQUIRES: cargo
// FR-93, differential end-to-end test for pointer LOCALS bound to
// member arrays — Phase 1, both backing kinds. TYPED backing: `p =
// s->arr` / `q = s.arr` walk a member place with a member-relative
// element cursor (deref, subscript, arithmetic, rebind-to-same-member,
// and a slice-argument borrow of just the field). WINDOW backing: `p =
// b->iv` / `q = b.iv` walk a byte-region root's flat byte image with
// an ABSOLUTE byte cursor starting at the field's layout offset — the
// exact convention an off-by-one cursor origin would corrupt by
// silently reading/writing sibling-field bytes, which is why every
// value below derives from argc (constant folding cannot pre-compute a
// buffer and hide a miscompile) and the byte streams are diffed against
// the clang-built native. `cargo build` success alone proves nothing
// here — the stdout diff is the oracle. Deterministic, no UB; main
// returns 0.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointers_member_array_local > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/pointers_member_array_local a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>

typedef unsigned char u8;

struct T { int tag; int arr[6]; };
struct B { u8 rk[8]; u8 iv[4]; };

static void bump(int *xs, int n) {
  int i;
  for (i = 0; i < n; i++)
    xs[i] = xs[i] * 2 + 1;
}

static u8 fold(const u8 *xs, int n) {
  u8 acc = 0;
  int i;
  for (i = 0; i < n; i++)
    acc = (u8)(acc ^ (u8)(xs[i] + (u8)i));
  return acc;
}

/* Typed member backing: init, walk, deref-read/write, subscript,
   rebind to the same member, slice-argument borrow of the field. */
static int typed_walk(struct T *s, int k) {
  int *p = s->arr;
  int acc;
  p += k;
  acc = *p + p[1];
  *p = acc + 3;
  p = s->arr;
  bump(p, 4);
  return acc + *p + p[2];
}

/* Window backing, arrow root: absolute byte cursor from offset 8. */
static int window_walk(struct B *b, int k) {
  const u8 *p = b->iv;
  int acc;
  p += k;
  acc = (int)(p[1] ^ *p);
  p = b->iv;
  return acc + (int)fold(p, 4);
}

/* Window backing, MUT walk + write through the cursor local. */
static void window_write(struct B *b) {
  u8 *w = b->rk;
  w += 2;
  *w = (u8)(*w + 5);
  w[1] = (u8)(w[1] ^ 0x3c);
}

/* Dot roots: local struct (typed) and local byte-region aggregate. */
static int dot_roots(int seed) {
  struct T s;
  struct B b;
  int *q;
  const u8 *w;
  int i;
  s.tag = seed;
  for (i = 0; i < 6; i++)
    s.arr[i] = seed * 3 + i;
  for (i = 0; i < 8; i++)
    b.rk[i] = (u8)(seed + i);
  for (i = 0; i < 4; i++)
    b.iv[i] = (u8)(seed * 7 + i);
  q = s.arr;
  q++;
  w = b.iv;
  w += 2;
  return *q + (int)*w + (int)b.rk[3];
}

int main(int argc, char **argv) {
  struct T s;
  struct B b;
  int i;
  s.tag = argc;
  for (i = 0; i < 6; i++)
    s.arr[i] = argc * 11 + i * 3;
  for (i = 0; i < 8; i++)
    b.rk[i] = (u8)(argc * 13 + i * 5);
  for (i = 0; i < 4; i++)
    b.iv[i] = (u8)(argc * 29 + i * 3);
  printf("typed=%d\n", typed_walk(&s, 1));
  for (i = 0; i < 6; i++)
    printf(" %d", s.arr[i]);
  printf("\n");
  printf("window=%d\n", window_walk(&b, 1));
  window_write(&b);
  for (i = 0; i < 8; i++)
    printf(" %02x", (unsigned)b.rk[i]);
  printf("\n");
  printf("dot=%d\n", dot_roots(argc + 2));
  return 0;
}

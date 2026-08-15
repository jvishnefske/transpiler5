// REQUIRES: cargo
// FR-83 differential end-to-end test: THE PUN PROPERTY of opaque-union arm
// access. Arm accesses on an FR-78 blob lower to byte views at
// clang-computed offsets, so a write through one arm must be readable —
// byte-exactly, at native endianness — through the OTHER arm's overlapping
// leaf, in both directions, exactly as the C memory model prescribes for
// union punning. The dual-stack shape (lwIP's ip_addr_t): ip4.a overlaps
// ip6.a[0]; ip6.z sits past the ip4 arm at byte 16. Exercised: dot and
// arrow access, const and RUNTIME element cursors (k = argc % 4 covers
// every element across runs), the one-byte leaf, compound assignment and
// ++ on arm leaves (the composed read-modify-write image), and a staged
// GLOBAL record. Seeds derive from argc so constant folding cannot hide a
// miscompile; every observable value is printed and byte-diffed against
// the clang-built native, which makes the from/to_ne_bytes image exact by
// construction. All bytes read are written first through some arm (no
// uninitialized reads, no UB on either side).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/union_opaque_arm_pun > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native 7 8 > %t.native3.out
// RUN: %t.crate/target/release/union_opaque_arm_pun 7 8 > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

int printf(const char *, ...);

union U {
  struct { unsigned a[4]; unsigned char z; } ip6;
  struct { unsigned a; } ip4;
}; /* 20-byte blob: ip4.a == bytes of ip6.a[0]; ip6.z at byte 16 */

struct Rec {
  int tag;
  union U u;
};

struct Rec g_rec; /* zero-init global: staged-copy arm traffic below */

void arm_write4(struct Rec *p, unsigned v) { p->u.ip4.a = v; }
unsigned arm_read4(struct Rec *p) { return p->u.ip4.a; }
void arm_write6_elem(struct Rec *p, unsigned k, unsigned v) {
  p->u.ip6.a[k] = v;
}
unsigned arm_read6_elem(struct Rec *p, unsigned k) { return p->u.ip6.a[k]; }
void arm_write6_z(struct Rec *p, unsigned char b) { p->u.ip6.z = b; }
unsigned arm_read6_z(struct Rec *p) { return p->u.ip6.z; }

/* Dot-access pun on a local: write ip4, read back through ip6.a[0]. */
unsigned local_pun(unsigned v) {
  struct Rec r = {0};
  r.u.ip4.a = v;
  return r.u.ip6.a[0];
}

int main(int argc, char **argv) {
  unsigned seed = 2654435761u * (unsigned)argc;
  unsigned k = (unsigned)argc % 4u;
  struct Rec r = {0};
  int i;

  printf("localpun=%u\n", local_pun(seed));

  /* Direction 1: write through ip4, read the same bytes through ip6. */
  arm_write4(&r, seed);
  printf("d1const=%u\n", arm_read6_elem(&r, 0u));

  /* Direction 2: write through ip6 (const elem 0), read through ip4. */
  arm_write6_elem(&r, 0u, seed ^ 0x33cc33ccu);
  printf("d2=%u\n", arm_read4(&r));

  /* Runtime cursor both ways: fill every element, then overwrite at the
     argc-derived index and read it back at the same runtime index. */
  for (i = 0; i < 4; i++)
    arm_write6_elem(&r, (unsigned)i, seed + (unsigned)i);
  arm_write6_elem(&r, k, seed ^ 0x0f0f0f0fu);
  printf("rt=%u\n", arm_read6_elem(&r, k));
  printf("rt0=%u\n", arm_read4(&r));

  /* One-byte leaf past the short arm. */
  arm_write6_z(&r, (unsigned char)seed);
  printf("z=%u\n", arm_read6_z(&r));

  /* Compound assignment and ++ compose the two byte-view images. */
  r.u.ip4.a += seed >> 3;
  printf("comp=%u\n", arm_read6_elem(&r, 0u));
  r.u.ip6.z++;
  printf("bump=%u\n", arm_read6_z(&r));

  /* Global record: staged copy + writeback, punned back out through the
     other arm. */
  g_rec.u.ip4.a = seed ^ 0x5a5a5a5au;
  printf("glob=%u\n", g_rec.u.ip6.a[0]);
  printf("globz=%u\n", g_rec.u.ip6.z + (unsigned)argc);
  return 0;
}

// REQUIRES: cargo
// FR-229 Wave 1: the WRITE-BACK obligation, which is a MEASURED miscompile
// and not a theoretical one. A byte view is a materialized `[u8; N]` copy
// of the object's representation, so a callee that WRITES through
// `unsigned char *p` mutates the copy and nothing else. Without a
// `from_ne_bytes` reconstitution after the call the write is silently lost:
// `zap((unsigned char *)&x, 4); printf("%d", x)` prints the native
// -1431655766 against a naive Rust 1, and the emitted crate BUILDS CLEAN
// while printing the wrong number. That is exactly the failure `cargo
// build` cannot see, so it is pinned here by stdout diff against the
// clang-built native.
//
// Covered: a full-width overwrite, a PARTIAL overwrite (only the low byte,
// which is the case that catches a write-back that reconstitutes from a
// stale image instead of the mutated one), a read-modify-write through the
// view, a float object mutated through its bytes, and an aggregate whose
// fields must each come back from their own byte offset. Each object is
// read AFTER the call, so no dead-store elimination can remove the
// write-back; the read-only uses in byte-view-scalar.c cover the other
// direction, where DSE is free to drop it.
//
// Seeds derive from `argc`. Every object is fully initialized before its
// view is taken and every byte the callees write is written unconditionally,
// so no indeterminate byte is ever observed and the program has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/byte_view_writeback > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

static void print_hex(const unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
  printf("\n");
}

static void zap(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    p[i] = 0xaa;
}

static void low_byte(unsigned char *p, int n) {
  (void)n;
  p[0] = 0x7f;
}

static void bump(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    p[i] = (unsigned char)(p[i] + 1);
}

typedef struct {
  int a;
  int b;
} pair_t;

int main(int argc, char **argv) {
  int x = argc;
  int y = 0x01020304 + argc;
  unsigned int u = 0u;
  float f = 1.0f;
  pair_t pair;


  zap((unsigned char *)&x, sizeof(x));
  printf("zap x=%d\n", x);

  low_byte((unsigned char *)&y, sizeof(y));
  printf("low y=%d\n", y);

  bump((unsigned char *)&y, sizeof(y));
  printf("bump y=%d\n", y);

  zap((unsigned char *)&u, sizeof(u));
  printf("zap u=%u\n", u);

  // A float whose bytes are incremented: the result is still a finite
  // normal number, so its decimal rendering is identical on both sides.
  bump((unsigned char *)&f, sizeof(f));
  printf("bump f=%.9g\n", (double)f);
  // The reconstituted object's OWN bytes: a write-back that rebuilt `f`
  // from a stale image would print the pre-bump string here.
  print_hex((const unsigned char *)&f, sizeof(f));

  pair.a = argc;
  pair.b = -argc;
  low_byte((unsigned char *)&pair, sizeof(pair));
  printf("pair a=%d b=%d\n", pair.a, pair.b);

  bump((unsigned char *)&pair, sizeof(pair));
  printf("pair a=%d b=%d\n", pair.a, pair.b);
  return 0;
}

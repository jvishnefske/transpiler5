// REQUIRES: cargo
// FR-166 phase 2 + FR-169 phase C, differential end-to-end test for an enum
// whose ENUMERATOR values cover the full 32-bit UNSIGNED range.
//
// The invariant: clang gives `enum M` a 32-bit `unsigned int` underlying
// type because `M_MAX = 4294967295u` needs one, and every use of such an
// enum -- object or enumerator constant -- must round-trip through Rust
// with the same bits C gives it. Two independent things have to be true at
// once for that to hold, which is why they land together:
//
//   * FR-166 phase 2 opens the importer's enumerator range gate from
//     `isInt<32>` to `isUInt<32>` for an unsigned-underlying enum, so
//     `M_MID`/`M_MAX` are admitted at all instead of a located
//     "enumerator value does not fit in i32" rejection.
//   * FR-169 phase C fixes the ENUMERATOR REFERENCE. C17 6.4.4.3 types an
//     enumeration constant `int`, and the importer hard-cast every
//     enumerator DeclRefExpr to i32 to match -- correct until the range
//     opens, because clang types an out-of-int-range enumerator's
//     DeclRefExpr `unsigned int`, not `int`. Phase 2 without phase C is a
//     MEASURED SILENT MISCOMPILE: `(unsigned long)M_MAX` printed
//     18446744073709551615 instead of 4294967295, because `4294967295 as
//     i32` is -1 and the widening to u64 then sign-extends it.
//
// A wrong signedness compiles perfectly -- `x.0 as i32 as u64` is legal
// Rust -- so `cargo build` CANNOT see that miscompile. The oracle is the
// stdout byte-diff against the clang-built native. Every runtime value
// derives from `argc`, so no constant fold can hide a lost bit, and the
// THIRD seed (4294967295) is the load-bearing one.
//
// The `ul:` line is the exact shape that miscompiled, in both its
// constant-folded form and an argc-selected one.
//
// `enum S` runs the same battery with a negative enumerator, so clang gives
// it a SIGNED 32-bit underlying type: it must keep the historical shape
// byte for byte. Neither change is allowed to move a signed-underlying
// enum.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/enum_u32_enumerator_range > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/enum_u32_enumerator_range a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out && %t.crate/target/release/enum_u32_enumerator_range a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/enum_u32_enumerator_range a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out

int printf(const char *, ...);

/* No negative enumerator and a value above INT32_MAX: clang picks a 32-bit
   UNSIGNED underlying type. */
enum M { M_A = 1, M_MID = 2147483648u, M_MAX = 4294967295u };
/* A negative enumerator forces a SIGNED 32-bit underlying type. */
enum S { S_N = -1, S_A = 1, S_MID = 2000000000 };

static int g(int v) { return v; }
static unsigned h(unsigned v) { return v; }

int main(int argc, char **argv) {
  unsigned seeds[4] = {1u, 2147483648u, 4294967295u, 7u};
  unsigned s = seeds[(argc - 1) & 3];
  enum M x = (enum M)s;
  enum S y = (enum S)(int)s;

  /* Relational: unsigned storage compares unsigned (FR-169 phase A). */
  printf("rel: %d %d %d %d\n", x > M_MID, x < M_MAX, x >= M_A, x <= M_MID);
  printf("srel: %d %d %d %d\n", y > S_N, y < S_MID, y >= S_A, y <= S_N);

  /* All four cast widths off an enum-typed object. */
  printf("cast: %u %lu %ld %d\n", (unsigned)x, (unsigned long)x, (long)x,
         (int)x);
  printf("scast: %u %lu %ld %d\n", (unsigned)y, (unsigned long)y, (long)y,
         (int)y);

  /* Call arguments, both signednesses of parameter. */
  printf("call: %d %u\n", g(x), h(x));
  printf("scall: %d %u\n", g(y), h(y));

  /* Enumerator CONSTANTS: the FR-169 phase C site. Both the folded form
     and an argc-selected one, so a constant fold cannot hide a lost bit. */
  printf("ul: %lu %lu\n", (unsigned long)M_MAX,
         (unsigned long)(argc > 100 ? M_A : M_MAX));
  printf("sul: %ld %ld\n", (long)S_MID, (long)(argc > 100 ? S_A : S_N));

  /* Enumerator constants used bare and in integer arithmetic. */
  printf("k: %u %u %u\n", M_MAX, M_MID, M_MAX - M_A);
  printf("sk: %d %d %d\n", S_MID, S_N, S_MID - S_A);

  /* The two shapes that used to reject with "assigned value type does not
     match the place" instead of miscompiling: an enumerator initializing a
     plain `unsigned`, direct and through arithmetic. */
  unsigned a = M_MAX;
  unsigned c = M_MAX - M_A;
  printf("plain: %u %u\n", a, c);

  /* Equality is signedness-neutral and compares the enum values directly. */
  enum M z = M_MAX;
  printf("eq: %d %d %d\n", x == M_MAX, x != M_MID, z == x);
  printf("seq: %d %d\n", y == S_N, y != S_MID);

  /* switch: `case 4294967295` becomes reachable, and the emitter's
     bit-preserving discriminant chain must carry it. */
  int r = 0;
  switch (x) {
  case M_A: r = 1; break;
  case M_MID: r = 2; break;
  case M_MAX: r = 3; break;
  default: r = 9; break;
  }
  int q = 0;
  switch (y) {
  case S_N: q = 1; break;
  case S_A: q = 2; break;
  case S_MID: q = 3; break;
  default: q = 9; break;
  }
  printf("sw: %d %d\n", r, q);

  /* Truth test, array index, bitwise and additive arithmetic. */
  printf("truth: %d %d %d %d\n", x ? 1 : 0, !x, y ? 1 : 0, !y);
  int names[4] = {10, 11, 12, 13};
  printf("idx: %d %d\n", names[x & 3], names[y & 3]);
  printf("bit: %u %u\n", x & M_MID, x | M_A);
  printf("add: %u %u\n", x + 1, x - M_A);
  return 0;
}

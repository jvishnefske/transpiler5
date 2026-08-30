// REQUIRES: cargo
// FR-169, differential end-to-end test for RELATIONAL comparison of
// enum-typed OBJECTS whose enum has no negative enumerator.
//
// The invariant: C17 6.7.2.2 makes an enumerated type compatible with an
// implementation-chosen integer type, and clang picks `unsigned int` when
// no enumerator is negative. The value an enum-typed OBJECT may hold is
// therefore the whole UNSIGNED range -- it is NOT bounded by the
// enumerator list, and an object holding 3000000000 is ordinary,
// well-defined C, not an out-of-range enumerator and not undefined
// behavior. So `<`, `<=`, `>` and `>=` on such an enum must compare
// UNSIGNED. Before FR-169 the importer forced both operands through an
// `as i32` cast and used `arith.cmpi slt`, which reads 3000000000 as
// -1294967296 and inverts every relational answer.
//
// A wrong signedness compiles perfectly -- `x.0 as i32 > y.0 as i32` is
// legal Rust -- so `cargo build` CANNOT see this miscompile. The oracle is
// the stdout byte-diff against the clang-built native. Every runtime value
// derives from `argc`, so no constant fold can hide a lost bit, and the
// FOURTH seed (3000000000, above INT32_MAX) is the load-bearing one:
// without it this test passes at HEAD and pins nothing.
//
// `enum S` runs the identical battery side by side. It has a negative
// enumerator, so clang gives it a SIGNED underlying type and it must keep
// the historical signed `arith.cmpi slt` shape byte for byte -- the fix is
// driven by the `unsigned_underlying` marker on the enum definition, not
// by a blanket switch to unsigned.
//
// The `s:` (switch), `i:` (array index) and `k:` (enumerator constants in
// integer arithmetic) lines are deliberate NON-regression probes: those
// sites were measured correct at HEAD and FR-169 must not move them.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/enum_unsigned_object_relational > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/enum_unsigned_object_relational a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out && %t.crate/target/release/enum_unsigned_object_relational a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/enum_unsigned_object_relational a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out

int printf(const char *, ...);

/* No negative enumerator: clang picks an UNSIGNED underlying type. */
enum U { U_Z = 0, U_A = 1, U_B = 2, U_BIG = 2000000000 };
/* A negative enumerator forces a SIGNED underlying type. */
enum S { S_N = -3, S_Z = 0, S_A = 1, S_BIG = 2000000000 };

int main(int argc, char **argv) {
  /* argc picks in-range values AND, at argc == 4, a value above INT32_MAX
     that `enum U`'s unsigned underlying type represents exactly. */
  unsigned seeds[4] = {0u, 1u, 2000000000u, 3000000000u};
  unsigned s = seeds[(argc - 1) & 3];
  enum U u = (enum U)s;
  enum S v = (enum S)(int)s;

  printf("u: %d %d %d %d %d %d\n", u < U_A, u <= U_A, u > U_A, u >= U_A,
         u == U_A, u != U_A);
  printf("uu: %d %d %d %d\n", u < U_BIG, u > U_BIG, u == U_BIG, u != U_BIG);
  printf("v: %d %d %d %d %d %d\n", v < S_A, v <= S_A, v > S_A, v >= S_A,
         v == S_A, v != S_A);
  printf("vv: %d %d %d %d\n", v < S_N, v > S_N, v == S_N, v != S_N);
  /* Truth tests: `!= 0` is signedness-neutral and must not move. */
  printf("t: %d %d %d %d\n", u ? 1 : 0, !u, v ? 1 : 0, !v);
  /* switch: the emitter's bit-preserving discriminant chain must not move. */
  int r = 0;
  switch (u) {
  case U_Z: r = 1; break;
  case U_A: r = 2; break;
  case U_B: r = 3; break;
  case U_BIG: r = 4; break;
  default: r = 9; break;
  }
  int q = 0;
  switch (v) {
  case S_N: q = 1; break;
  case S_Z: q = 2; break;
  case S_A: q = 3; break;
  case S_BIG: q = 4; break;
  default: q = 9; break;
  }
  printf("s: %d %d\n", r, q);
  /* Array index by an in-range enum: must not move. */
  enum U idx = (enum U)((argc - 1) & 3);
  enum S sidx = (enum S)((argc - 1) & 3);
  int names[5] = {10, 11, 12, 13, 14};
  printf("i: %d %d\n", names[idx], names[sidx]);
  /* Enum compared against an int-derived enum value. */
  int iv = (int)s;
  printf("m: %d %d\n", u > (enum U)iv, v > (enum S)iv);
  /* Enumerator constants in integer arithmetic: C17 6.4.4.3 types an
     enumeration CONSTANT `int`, so these stay signed and must not move. */
  printf("k: %d %u %d\n", U_A - U_B, (unsigned)(S_N - S_A), U_BIG / 2);
  return 0;
}

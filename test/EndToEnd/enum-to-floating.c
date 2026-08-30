// REQUIRES: cargo
// FR-169 Phase B, differential end-to-end test for converting an
// enum-typed value to a FLOATING type.
//
// C puts no integer intermediary in the AST between an enum operand and a
// floating destination -- `CK_IntegralToFloating` sits directly on the
// enum-typed subexpression. Before FR-169 the importer handed that
// enum-typed value straight to `arith.sitofp`, which rejected it with a
// raw op-verifier error ("operand #0 must be signless-fixed-width-
// integer-like, but got '!emitrust.enum<...>'"), UNLOCATED for the
// implicit form. That violates the repo's rejection contract twice over:
// an ordinary, well-defined C construct was refused, and the refusal
// carried no source location.
//
// The conversion must also pick the right SIGNEDNESS. An enum with no
// negative enumerator has an unsigned underlying type, so its value
// converts as unsigned: 3000000000 becomes 3000000000.0, not
// -1294967296.0. That distinction is invisible to `cargo build` -- both
// spellings are legal Rust -- so the oracle is the stdout byte-diff
// against the clang-built native, with every value selected by `argc`.
//
// Both flavors run side by side and at 32- and 64-bit widths: `M`/`WU`
// have unsigned underlying types, `S`/`WS` signed ones.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/enum_to_floating > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/enum_to_floating a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out && %t.crate/target/release/enum_to_floating a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/enum_to_floating a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out

int printf(const char *, ...);

enum M { M_A = 1, M_B = 2 };                /* unsigned underlying, 32-bit */
enum S { S_N = -1, S_A = 1 };               /* signed underlying, 32-bit */
enum WU { WU_A = 0, WU_B = 5000000000ULL }; /* unsigned underlying, 64-bit */
enum WS { WS_A = -1, WS_B = 5000000000LL }; /* signed underlying, 64-bit */

int main(int argc, char **argv) {
  unsigned useed[4] = {1u, 2u, 3000000000u, 4000000000u};
  int sseed[4] = {1, -1, -2000000000, 0};
  unsigned long long wuseed[4] = {0ULL, 5000000000ULL, 9000000000ULL, 3ULL};
  long long wsseed[4] = {-1LL, 5000000000LL, -5000000000LL, 0LL};
  int k = (argc - 1) & 3;

  enum M x = (enum M)useed[k];
  enum S y = (enum S)sseed[k];
  enum WU wu = (enum WU)wuseed[k];
  enum WS ws = (enum WS)wsseed[k];

  /* Explicit casts. */
  printf("cast %.1f %.1f %.1f %.1f\n", (double)x, (double)y, (double)wu,
         (double)ws);
  /* Implicit conversions in an initializer -- this is the form whose
     op-verifier rejection carried no source location. */
  double dx = x;
  double dy = y;
  double dwu = wu;
  double dws = ws;
  printf("init %.1f %.1f %.1f %.1f\n", dx, dy, dwu, dws);
  /* Float destination, then widened back for printing. */
  float fx = (float)x;
  float fy = (float)y;
  printf("flt %.1f %.1f\n", (double)fx, (double)fy);
  /* Arithmetic in a floating context. */
  printf("mix %.2f %.2f\n", x / 2.0, y / 2.0);
  return 0;
}

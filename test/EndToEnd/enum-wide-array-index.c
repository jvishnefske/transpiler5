// REQUIRES: cargo
// FR-166 x FR-149, differential end-to-end test for a 64-BIT-UNDERLYING enum
// used as an ARRAY SUBSCRIPT. FR-149 normalizes an enum-typed index to its
// discriminant at one seam (`emitSubscriptIndexRValue`), and that seam used
// to force `i32` unconditionally. With a `_SD_ENUM_FORCE_S64`-style enum the
// discriminant is `i64`, so the seam has to follow the def's width -- a
// mismatch there is either a verifier abort (nothing emitted) or, worse, a
// silent conversion that reads a DIFFERENT ELEMENT while compiling cleanly.
// `cargo build` cannot see that, so the oracle is the stdout byte-diff
// against the clang-built native, with every index derived from `argc`: the
// four RUN lines walk the enum across all four in-range enumerators, so an
// off-by-one or a dropped conversion moves the printed bytes instead of
// hiding behind a constant fold.
//
// The shapes span the distinct subscript construction sites: a plain read, a
// write position, both levels of a 2-D array, and a subscript through a
// decomposed POINTER (which folds the index into a flat cursor).
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/enum_wide_array_index > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/enum_wide_array_index a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out && %t.crate/target/release/enum_wide_array_index a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/enum_wide_array_index a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out

int printf(const char *, ...);

/* The last enumerator is the force-to-64-bits one; the first four are the
   real, in-range values the subscripts use. */
enum JT {
  JT_A = 0,
  JT_B = 1,
  JT_C = 2,
  JT_D = 3,
  _JT_FORCE_S64 = 5000000000LL,
};

static const int tbl[4] = {10, 20, 30, 40};
static const int grid[4][2] = {{1, 2}, {3, 4}, {5, 6}, {7, 8}};

int main(int argc, char **argv) {
  enum JT t = (enum JT)(argc & 3);      /* 1, 2, 3, 0 across the RUN lines */
  enum JT u = (enum JT)((argc + 1) & 3);

  printf("t=%lld u=%lld\n", (long long)t, (long long)u);

  /* The reproducer itself: read a table at a wide-enum index. */
  printf("tbl=%d %d\n", tbl[t], tbl[u]);

  /* Write position: the store must land in the element the enum names. */
  int slot[4] = {0, 0, 0, 0};
  slot[t] = argc * 7;
  slot[u] = slot[u] + argc;
  for (int i = 0; i < 4; ++i)
    printf("slot[%d]=%d\n", i, slot[i]);

  /* Both levels of a 2-D array, the wide enum on each index in turn. */
  printf("outer=%d\n", grid[t][argc & 1]);
  printf("inner=%d\n", grid[argc & 3][(argc + 1) & 1]);

  /* Subscript through a decomposed pointer. */
  const int *p = tbl;
  printf("ptr=%d %d\n", p[t], p[u]);

  /* The index in arithmetic alongside its own subscript, so a lost
     conversion cannot cancel out. */
  printf("mix=%lld\n", (long long)(tbl[t] * 2 - (int)t + tbl[u]));
  return 0;
}

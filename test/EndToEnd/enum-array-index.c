// REQUIRES: cargo
// FR-149, differential end-to-end test for an ENUM-VALUED ARRAY SUBSCRIPT
// (systemd `src/basic/log.c:1427`, `return log_target_max_level[target];`).
// A named-enum index used to abort the translation unit with a verifier
// error, so nothing was emitted at all; the fix normalizes the index to its
// i32 discriminant with an `emitrust.cast`. That is CODEGEN, and a wrong
// cast reads a DIFFERENT ELEMENT while compiling perfectly -- `cargo build`
// cannot see it. So the oracle here is the stdout byte-diff against the
// clang-built native, and every index below derives from `argc`: the four
// RUN lines walk the enum across all four of its enumerators, so an
// off-by-one, a sign error or a dropped conversion changes the printed
// bytes instead of hiding behind a constant fold.
//
// The shapes span the distinct subscript construction sites the importer
// has: a plain read, a write position, both levels of a 2-D array, a
// subscript through a decomposed POINTER (which folds the index into a
// flat cursor and used to reject with "unsupported subscript index type"),
// and a SIGNED-underlying enum (a negative enumerator forces `i32` storage,
// where an unsigned-underlying enum stores `u32` and the normalizing cast
// is a real conversion rather than an identity).
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/enum_array_index > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/enum_array_index a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out && %t.crate/target/release/enum_array_index a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/enum_array_index a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out

int printf(const char *, ...);

typedef enum LogTarget {
  T_CONSOLE = 0,
  T_KMSG = 1,
  T_JOURNAL = 2,
  T_NULL = 3
} LogTarget;

/* A negative enumerator forces a SIGNED underlying type. */
enum Temp { Cold = -2, Mild = 0, Warm = 1, Hot = 2 };

static const int target_max_level[4] = {11, 22, 33, 44};
static const int grid[4][3] = {
    {100, 101, 102}, {110, 111, 112}, {120, 121, 122}, {130, 131, 132}};
static const int temp_scale[3] = {7, 8, 9};

/* argc -> enumerator, with no constant an optimizer can propagate. */
static LogTarget pick(int seed) {
  switch (seed & 3) {
  case 0:
    return T_CONSOLE;
  case 1:
    return T_KMSG;
  case 2:
    return T_JOURNAL;
  default:
    return T_NULL;
  }
}

static enum Temp pick_temp(int seed) {
  switch (seed % 3) {
  case 0:
    return Mild;
  case 1:
    return Warm;
  default:
    return Hot;
  }
}

int main(int argc, char **argv) {
  int seed = argc; /* 1, 2, 3, 4 across the four RUN lines */
  LogTarget t = pick(seed);
  LogTarget t2 = pick(seed + 1);
  enum Temp u = pick_temp(seed);

  /* The reproducer itself: read a table at an enum index. */
  printf("t=%d max=%d\n", (int)t, target_max_level[t]);
  printf("t2=%d max2=%d\n", (int)t2, target_max_level[t2]);

  /* Write position: the store must land in the element the enum names. */
  int slot[4] = {0, 0, 0, 0};
  slot[t] = seed * 7;
  slot[t2] = slot[t2] + seed;
  for (int i = 0; i < 4; ++i)
    printf("slot[%d]=%d\n", i, slot[i]);

  /* Both levels of a 2-D array, enum on the outer and on the inner index. */
  printf("outer=%d\n", grid[t][seed % 3]);
  printf("inner=%d\n", grid[seed & 3][u]);
  printf("both=%d\n", grid[t][u]);

  /* Subscript through a decomposed pointer. */
  const int *p = target_max_level;
  printf("ptr=%d\n", p[t]);
  printf("ptr2=%d\n", p[t2]);

  /* Signed-underlying enum as an index. */
  printf("u=%d temp=%d\n", (int)u, temp_scale[u]);

  /* The enum index in arithmetic alongside its own subscript, so a lost
     conversion cannot cancel out. */
  printf("mix=%d\n", target_max_level[t] * 2 - (int)t + temp_scale[u]);
  return 0;
}

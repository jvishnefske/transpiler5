// REQUIRES: cargo
// C99-11/C99-12: aggregate initializer list differential end-to-end test:
// transpile to a cargo crate, build it, and compare its stdout against the
// natively compiled C program. Adversarial value shapes on purpose:
// negative values including INT_MIN/LONG_MIN, width-extreme u32 elements,
// f64 (including -0.0), partial initialization with implicit zeroing,
// `[i] =` and `.field =` designators, a designated initializer overwriting
// a positional one, nested array-of-struct and struct-with-array lists,
// non-constant block-scope elements, and a partially initialized static
// local. main returns 0 and reports everything via printf, so lit's
// per-command exit-code checking covers both runs and diff covers the
// observable behavior. The program is deterministic and has no UB; every
// printf carries at most one side-effect-free argument list.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/aggregate_init > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int gi[5] = {-1, -2147483647 - 1, 2147483647, 4};
unsigned int gu[3] = {4000000000u, 7u, 0xFFFFFFFFu};
double gd[3] = {-2.5, 1e100, -0.0};
long gl[3] = {-9223372036854775807L - 1, 9223372036854775807L, -5};
const int gc[4] = {[2] = -9, [0] = 3};
/* Designated initializer overwriting a positional one at file scope. */
int gov[4] = {1, 2, [1] = -7, [3] = 8};

struct P {
  int x;
  unsigned int u;
  double d;
};
struct P gp = {-7, 4000000000u, -1.25};
struct P gpartial = {.d = 2.5};
struct P garr[3] = {{1, 2u, 3.0}, [2] = {.u = 9u}};

struct WithArr {
  int tag;
  int vals[4];
};
struct WithArr gwa = {5, {-1, -2, -3}};

int next_seed(void) {
  static int seeds[3] = {10, -20};
  static int idx;
  int v = seeds[idx];
  idx = idx + 1;
  return v;
}

int main(void) {
  for (int i = 0; i < 5; i++) printf("gi[%d]=%d\n", i, gi[i]);
  for (int i = 0; i < 3; i++) printf("gu[%d]=%ld\n", i, (long)gu[i]);
  for (int i = 0; i < 3; i++) printf("gd[%d]=%f\n", i, gd[i]);
  for (int i = 0; i < 3; i++) printf("gl[%d]=%ld\n", i, gl[i]);
  for (int i = 0; i < 4; i++) printf("gc[%d]=%d\n", i, gc[i]);
  for (int i = 0; i < 4; i++) printf("gov[%d]=%d\n", i, gov[i]);
  printf("gp=%d %ld %f\n", gp.x, (long)gp.u, gp.d);
  printf("gpartial=%d %ld %f\n", gpartial.x, (long)gpartial.u, gpartial.d);
  for (int i = 0; i < 3; i++)
    printf("garr[%d]=%d %ld %f\n", i, garr[i].x, (long)garr[i].u, garr[i].d);
  printf("gwa=%d %d %d %d %d\n", gwa.tag, gwa.vals[0], gwa.vals[1],
         gwa.vals[2], gwa.vals[3]);

  /* Block scope: partial, designated, overwrite, nested, non-constant
     elements derived from a runtime value. */
  int n = gi[3];
  int a[5] = {-1, n * 2, [4] = -2147483647 - 1};
  for (int i = 0; i < 5; i++) printf("a[%d]=%d\n", i, a[i]);
  unsigned int ua[3] = {4000000000u, n};
  for (int i = 0; i < 3; i++) printf("ua[%d]=%ld\n", i, (long)ua[i]);
  double da[3] = {[1] = -2.5, [0] = 1e-3};
  for (int i = 0; i < 3; i++) printf("da[%d]=%f\n", i, da[i]);
  /* Designated initializer overwriting a positional one at block scope. */
  int ow[3] = {11, 22, [0] = -33};
  for (int i = 0; i < 3; i++) printf("ow[%d]=%d\n", i, ow[i]);
  struct P lp = {.u = 123u, .x = -n};
  printf("lp=%d %ld %f\n", lp.x, (long)lp.u, lp.d);
  struct P larr[2] = {[1] = {.d = -8.5, .x = n}};
  for (int i = 0; i < 2; i++)
    printf("larr[%d]=%d %ld %f\n", i, larr[i].x, (long)larr[i].u, larr[i].d);
  struct WithArr lwa = {n, {[2] = -n}};
  printf("lwa=%d %d %d %d %d\n", lwa.tag, lwa.vals[0], lwa.vals[1],
         lwa.vals[2], lwa.vals[3]);
  int s0 = next_seed();
  int s1 = next_seed();
  int s2 = next_seed();
  printf("seeds=%d %d %d\n", s0, s1, s2);
  return 0;
}

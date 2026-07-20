// REQUIRES: cargo
// W3.3 G5/G6 EndToEnd differential: an external-linkage region API
// (a shared global `region` plus an externally visible `region_fill`)
// consumed from two translation units. The W3.3 whole-program cell-slice
// merge promotes `region_fill`'s parameter to a generic `&[Cell<i32>]`,
// with each call site (both TUs) binding `region` via `emitrust.global_cells`.
// Definition-before-call ordering (main first) is what enables the
// promotion; this differential proves the promoted cross-TU cell-slice
// lowering is RUNTIME identical to the clang-linked native program.
// Companion TU lives in Inputs/ (not discovered as a test).
// RUN: emitrust-cc --emit=crate %s %S/Inputs/multi-tu-gate-g5-region-api-other.c -o %t.crate --crate-name g5_region --build
// RUN: clang -std=c11 %s %S/Inputs/multi-tu-gate-g5-region-api-other.c -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/g5_region > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int region[8];

void region_fill(int *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    p[i] = i;
}

// Defined in the companion TU; also fills the same external `region`.
int reset_region(void);

int main(void) {
  region_fill(region, 8);
  int a = region[3];
  int b = reset_region();
  printf("region3=%d reset0=%d\n", a, b);
  return 0;
}

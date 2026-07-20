// Companion for multi-tu-gate-g5-cellslice-global-external.c: a realistic
// second consumer of the same externally visible region API, calling the
// same `region_fill` on the same externally visible `region` from a
// DIFFERENT translation unit — the shape that makes `region`'s
// external linkage a genuine whole-program fact, not a companion-file
// technicality.
extern int region[8];
void region_fill(int *p, int n);

int reset_region(void) {
  region_fill(region, 8);
  return region[0];
}

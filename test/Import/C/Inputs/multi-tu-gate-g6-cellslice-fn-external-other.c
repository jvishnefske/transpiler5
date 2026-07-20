// Companion for multi-tu-gate-g6-cellslice-fn-external.c: a realistic
// second caller of the same externally visible `sum4`, passing its OWN
// internal-linkage global array — the shape that makes `sum4`'s external
// linkage a genuine whole-program fact (this TU's global is invisible to
// the main file) WITHOUT introducing a genuinely-incompatible base (see
// multi-tu-gate-g6-cellslice-fn-negative.c for the local-argument
// counterexample that must stay rejected even after W3.2).
static int B[4] = {5, 6, 7, 8};
int sum4(int *a);

int use_sum4(void) { return sum4(B); }

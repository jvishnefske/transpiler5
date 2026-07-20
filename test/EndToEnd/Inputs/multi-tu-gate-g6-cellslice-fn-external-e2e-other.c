// Companion TU for test/EndToEnd/multi-tu-gate-g6-cellslice-fn-external.c:
// calls the SAME externally visible cell-slice function `sum4` on its OWN
// internal-linkage global `B` — a DIFFERENT global than the main TU's `A`.
// The generic `&[Cell<i32>]` parameter backs each TU's own global via a
// distinct `global_cells` region. Excluded from discovery by
// config.excludes = ["Inputs"].
static int B[4] = {5, 6, 7, 8};
int sum4(int *a);

int use_sum4(void) { return sum4(B); }

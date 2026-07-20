// Companion for multi-tu-gate-g5-cellslice-global-negative.c: calls the
// SAME externally visible `region_fill` on a SECOND, DIFFERENT externally
// visible array (`other_region`, not `region`) — the genuine multi-base
// counterexample a future whole-program cell-slice merge must still
// refuse to unify into one class.
int other_region[8];
void region_fill(int *p, int n);

int reset_other_region(void) {
  region_fill(other_region, 8);
  return other_region[0];
}

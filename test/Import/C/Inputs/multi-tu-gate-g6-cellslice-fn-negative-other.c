// Companion for multi-tu-gate-g6-cellslice-fn-negative.c: calls the SAME
// externally visible `sum4` with a LOCAL array — the genuinely
// incompatible cross-TU call site that must keep `sum4` off the
// cell-slice path even after a whole-program call-site merge exists.
int sum4(int *a);

int use_sum4(void) {
  int local[4] = {5, 6, 7, 8};
  return sum4(local);
}

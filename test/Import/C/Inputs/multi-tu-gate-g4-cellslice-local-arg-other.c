// Companion for multi-tu-gate-g4-cellslice-local-arg.c: calls the SAME
// externally visible `proc` with a LOCAL array this TU owns — the
// genuinely incompatible cross-TU call site that must keep `proc` off
// the cell-slice path even after a whole-program call-site merge exists.
void proc(int *p);

int local_call(void) {
  int loc[4];
  proc(loc);
  return loc[0];
}

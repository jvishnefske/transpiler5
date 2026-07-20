// Companion for multi-tu-gate-g3-owner-multibase.c: calls the SAME `fill`
// on a SECOND, unrelated local array `b` — the genuine multi-base
// counterexample that must keep `fill` off the owner-struct path even
// once whole-program call-site visibility exists.
int fill(int *p, int n);

int use_fill(void) {
  int b[4];
  return fill(b, 4);
}

// Companion for multi-tu-gate-g7-fnptr-devirt-negative.c: genuinely
// reassigns the SAME `fp` to a DIFFERENT target from another TU — the
// real divergence that must keep `fp` off the devirtualization path
// forever, independent of whole-program visibility.
extern int (*fp)(int, int);
int mul(int a, int b) { return a * b; }

void rebind(void) { fp = &mul; }

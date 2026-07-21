// Companion TU for test/EndToEnd/multi-tu-gate-g1-fnptr-result-erasure.c: an
// unrelated function that only forces the >=2-TU project import path, so the
// G1 erasure is a genuine whole-program decision. Excluded from discovery by
// config.excludes = ["Inputs"].
int unrelated_g1(void) { return 7; }

// Companion TU for test/EndToEnd/multi-tu-gate-g8-ptr-global-external.c:
// an unrelated function that only forces the >=2-TU project import path,
// so `g`'s external linkage is a genuine whole-program fact. Excluded from
// discovery by config.excludes = ["Inputs"].
int unrelated_g8(void) { return 7; }

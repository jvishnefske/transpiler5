// Companion TU for test/EndToEnd/multi-tu-gate-g3-owner-fallback.c: a
// trivial function that forces the >=2-TU project import path without ever
// calling fill/sum/total, so those three externally visible functions are
// referenced only by the main TU and the W3.3 G3 relaxation promotes them
// to the Owner_main_arr methods. Excluded from test discovery by
// config.excludes = ["Inputs"].
int unrelated_g3(void) { return 42; }

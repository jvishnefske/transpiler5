// Unrelated companion TU for multi-tu-gate-g3-owner-fallback.c: only
// forces the >=2-TU / project import path (soleTranslationUnit=false),
// which is what disqualifies `fill`/`sum`/`total`'s owner-struct
// promotion in the main file.
int unrelated_g3(void) { return 0; }

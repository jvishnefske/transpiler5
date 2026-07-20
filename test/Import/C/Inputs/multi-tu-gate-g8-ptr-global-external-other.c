// Unrelated companion TU for multi-tu-gate-g8-ptr-global-external.c: only
// forces the >=2-TU / project import path (soleTranslationUnit=false),
// which is what triggers `g`'s external-linkage rejection in the main
// file. Deliberately does not reference `g` at all (see
// multi-tu-gate-g8-ptr-global-shared-header.c for the case where a
// companion DOES forward-declare it with `extern`).
int unrelated_g8(void) { return 0; }

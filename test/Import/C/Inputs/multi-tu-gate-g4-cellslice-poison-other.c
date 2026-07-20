// Unrelated companion TU for multi-tu-gate-g4-cellslice-poison.c: only
// forces the >=2-TU / project import path (soleTranslationUnit=false),
// which is what triggers `pub_fn`'s preemptive cell-slice poison in the
// main file.
int unrelated_g4(void) { return 0; }

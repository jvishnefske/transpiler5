// Unrelated companion TU for multi-tu-gate-g1-fnptr-result-erasure.c.
// Excluded from test discovery by config.excludes = ["Inputs"]. Its only
// job is to push the import from the single-file (`importC`,
// soleTranslationUnit always true) path to the project (`importCProject`,
// soleTranslationUnit = numASTs==1) path, so it deliberately shares no
// declarations with the companion test.
int unrelated_g1(void) { return 0; }

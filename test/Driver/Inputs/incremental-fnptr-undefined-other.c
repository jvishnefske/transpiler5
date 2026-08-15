/* Companion TU for incremental-fnptr-undefined.c: one perfectly
 * translatable externally visible function, so the fixture is genuinely
 * multi-TU and the surviving cross-TU call chain proves the drop stayed
 * per-item. */
int offset(int x) { return x + 4; }

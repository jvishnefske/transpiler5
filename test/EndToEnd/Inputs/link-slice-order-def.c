/* FR-158 phase 2 companion TU: the DEFINITION. `printf("%s", text)` makes
   the importer classify `text` as a slice, so a TU that only saw the
   prototype disagrees with this signature. */

int printf(const char *, ...);

void note(const char *text, int line) { printf("%s %d\n", text, line); }

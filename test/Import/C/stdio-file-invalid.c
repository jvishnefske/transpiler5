// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/seek.c 2>&1 | FileCheck %s --check-prefix=FSEEK
// RUN: not emitrust-import-c %t/tell.c 2>&1 | FileCheck %s --check-prefix=FTELL
// RUN: not emitrust-import-c %t/rewindcase.c 2>&1 | FileCheck %s --check-prefix=REWIND
// RUN: not emitrust-import-c %t/mode-append.c 2>&1 | FileCheck %s --check-prefix=MODEA
// RUN: not emitrust-import-c %t/fprintf-stream.c 2>&1 | FileCheck %s --check-prefix=FPRINTF
// RUN: not emitrust-import-c %t/fread-wide.c 2>&1 | FileCheck %s --check-prefix=FREADN
// RUN: not emitrust-import-c %t/fwrite-wide.c 2>&1 | FileCheck %s --check-prefix=FWRITEN
// RUN: not emitrust-import-c %t/escape.c 2>&1 | FileCheck %s --check-prefix=ESCAPE
// RUN: not emitrust-import-c %t/struct-member.c 2>&1 | FileCheck %s --check-prefix=STRUCTFILE
// RUN: not emitrust-import-c %t/global-file.c 2>&1 | FileCheck %s --check-prefix=GLOBALFILE
// RUN: not emitrust-import-c %t/array-file.c 2>&1 | FileCheck %s --check-prefix=ARRAYFILE

// C99-48 (00187) rejections: the FILE* slice covers exactly sequential
// byte-wise I/O on an owned, function-local handle opened with mode "r"
// or "w". Everything else stays a located rejection:
//   - file positioning (fseek/ftell/rewind);
//   - fopen modes other than "r"/"w" (e.g. "a");
//   - fprintf to a real FILE* stream variable (the only accepted fprintf
//     remains the devirtualized stdout-swallow form);
//   - fread/fwrite with element size != 1 (byte-wise only);
//   - a FILE* crossing into a user-defined function (no escapes, v1);
//   - FILE* stored anywhere but a function-local variable (struct
//     member, global, array).
// The wordings pinned below are authored by this RED phase and are
// binding on GREEN.

//--- seek.c
#include <stdio.h>
int main(void) {
  FILE *f = fopen("t13_seek.txt", "r");
  fseek(f, 0, 0);
  fclose(f);
  return 0;
}
// FSEEK: seek.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: file positioning 'fseek' (FILE* streams are sequential-only)

//--- tell.c
#include <stdio.h>
int main(void) {
  FILE *f = fopen("t13_tell.txt", "r");
  ftell(f);
  fclose(f);
  return 0;
}
// FTELL: tell.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: file positioning 'ftell' (FILE* streams are sequential-only)

//--- rewindcase.c
#include <stdio.h>
int main(void) {
  FILE *f = fopen("t13_rewind.txt", "r");
  rewind(f);
  fclose(f);
  return 0;
}
// REWIND: rewindcase.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: file positioning 'rewind' (FILE* streams are sequential-only)

//--- mode-append.c
#include <stdio.h>
int main(void) {
  FILE *f = fopen("t13_append.txt", "a");
  fclose(f);
  return 0;
}
// MODEA: mode-append.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: fopen mode "a" (only "r" and "w" are supported)

//--- fprintf-stream.c
#include <stdio.h>
int main(void) {
  FILE *f = fopen("t13_fprintf.txt", "w");
  fprintf(f, "%d\n", 42);
  fclose(f);
  return 0;
}
// FPRINTF: fprintf-stream.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: fprintf to a FILE* stream (only the devirtualized stdout form is supported)

//--- fread-wide.c
#include <stdio.h>
int main(void) {
  int buf[4];
  FILE *f = fopen("t13_fread_wide.txt", "r");
  fread(buf, 4, 4, f);
  fclose(f);
  return 0;
}
// FREADN: fread-wide.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: fread element size other than 1 (FILE* I/O is byte-wise)

//--- fwrite-wide.c
#include <stdio.h>
int main(void) {
  int buf[4];
  FILE *f = fopen("t13_fwrite_wide.txt", "w");
  fwrite(buf, 4, 4, f);
  fclose(f);
  return 0;
}
// FWRITEN: fwrite-wide.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: fwrite element size other than 1 (FILE* I/O is byte-wise)

//--- escape.c
#include <stdio.h>
static int read_first(FILE *g) { return fgetc(g); }
int main(void) {
  FILE *f = fopen("t13_escape.txt", "r");
  int c = read_first(f);
  fclose(f);
  return c;
}
// ESCAPE: escape.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: FILE* cannot cross a user-defined function boundary

//--- struct-member.c
#include <stdio.h>
struct holder {
  FILE *fp;
};
int main(void) {
  struct holder h;
  h.fp = fopen("t13_struct.txt", "r");
  fclose(h.fp);
  return 0;
}
// STRUCTFILE: struct-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: FILE* is only supported as a function-local variable

//--- global-file.c
#include <stdio.h>
FILE *g_file;
int main(void) {
  g_file = fopen("t13_global.txt", "r");
  fclose(g_file);
  return 0;
}
// GLOBALFILE: global-file.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: FILE* is only supported as a function-local variable

//--- array-file.c
#include <stdio.h>
int main(void) {
  FILE *streams[2];
  streams[0] = fopen("t13_array.txt", "r");
  fclose(streams[0]);
  return 0;
}
// ARRAYFILE: array-file.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: FILE* is only supported as a function-local variable

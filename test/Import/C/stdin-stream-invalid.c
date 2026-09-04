// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/close-stdin.c 2>&1 | FileCheck %s --check-prefix=CLOSE
// RUN: not emitrust-import-c %t/bind-stdin.c 2>&1 | FileCheck %s --check-prefix=BIND
// RUN: not emitrust-import-c %t/getc-stdout.c 2>&1 | FileCheck %s --check-prefix=GETCOUT
// RUN: not emitrust-import-c %t/gets-stderr.c 2>&1 | FileCheck %s --check-prefix=GETSERR
// RUN: not emitrust-import-c %t/read-stdout.c 2>&1 | FileCheck %s --check-prefix=READOUT
// RUN: not emitrust-import-c %t/write-stdin.c 2>&1 | FileCheck %s --check-prefix=WRITEIN
// RUN: not emitrust-import-c %t/scanf-stdout.c 2>&1 | FileCheck %s --check-prefix=SCANOUT
// RUN: not emitrust-import-c %t/seek-stdin.c 2>&1 | FileCheck %s --check-prefix=SEEK
// RUN: not emitrust-import-c %t/rewind-stdin.c 2>&1 | FileCheck %s --check-prefix=REWIND
// RUN: not emitrust-import-c %t/unget-stdin.c 2>&1 | FileCheck %s --check-prefix=UNGET
// RUN: not emitrust-import-c %t/feof-stdin.c 2>&1 | FileCheck %s --check-prefix=FEOF
// RUN: not emitrust-import-c %t/ferror-stdin.c 2>&1 | FileCheck %s --check-prefix=FERROR

// FR-183: `stdin` is modelled as a STATELESS `__EmitrustFile::Stdin` unit
// variant, and the importer therefore materializes a FRESH TEMPORARY handle
// at every use site. That is exactly what dissolves the ownership problem a
// shared global FILE* would raise -- and it is also why a handful of shapes
// MUST be refused rather than admitted.
//
// THE ONE THAT WOULD MISCOMPILE IS `fclose(stdin)`. The close helper writes
// `__EmitrustFile::Null` through its `&mut`, so closing a per-use temporary
// would be a SILENT NO-OP: the C program would go on reading a stream it had
// closed, and the emitted crate would answer bytes instead of undefined
// behavior. Nothing about that failure is loud, so it is a located rejection
// here. `FILE *f = stdin;` is the same hazard one indirection away (a
// variable holding the handle is closable, seekable and aliasable), and
// `fseek`/`rewind`/`ungetc`/`feof`/`ferror` all read or write stream state
// the stateless model does not carry.
//
// The write direction is refused symmetrically: only sequential READS of
// `stdin` are in the slice, so a reader on `stdout`/`stderr` and a writer on
// `stdin` are both located rejections. (Formatted output to `stdout` keeps
// its own devirtualized-printf path; this is about the FILE* handle surface.)
//
// The wordings for the stdin/stdout/stderr handle rejection are authored by
// this RED phase; the positioning and system-header wordings below are
// PRE-EXISTING and are pinned here to prove FR-183 did not loosen them.

//--- close-stdin.c
#include <stdio.h>
int main(void) {
  fclose(stdin);
  return 0;
}
// CLOSE: close-stdin.c:3:10: error: unsupported: 'stdin' as a FILE* stream here (only sequential reads of 'stdin' are supported)

//--- bind-stdin.c
#include <stdio.h>
int main(void) {
  FILE *f = stdin;
  return fgetc(f);
}
// A variable holding the handle is closable and aliasable, so the binding
// itself is refused (by the pre-existing pointer machinery).
// BIND: bind-stdin.c:3:13: error: unsupported: copying a global pointer variable

//--- getc-stdout.c
#include <stdio.h>
int main(void) { return fgetc(stdout); }
// GETCOUT: getc-stdout.c:2:31: error: unsupported: 'stdout' as a FILE* stream here (only sequential reads of 'stdin' are supported)

//--- gets-stderr.c
#include <stdio.h>
int main(void) {
  char b[8];
  if (fgets(b, 8, stderr) == NULL)
    return 1;
  return b[0];
}
// GETSERR: gets-stderr.c:4:19: error: unsupported: 'stderr' as a FILE* stream here (only sequential reads of 'stdin' are supported)

//--- read-stdout.c
#include <stdio.h>
int main(void) {
  char b[8];
  fread(b, 1, 8, stdout);
  return b[0];
}
// READOUT: read-stdout.c:4:18: error: unsupported: 'stdout' as a FILE* stream here (only sequential reads of 'stdin' are supported)

//--- write-stdin.c
#include <stdio.h>
int main(void) {
  char b[8];
  b[0] = 'x';
  fwrite(b, 1, 1, stdin);
  return 0;
}
// WRITEIN: write-stdin.c:5:19: error: unsupported: 'stdin' as a FILE* stream here (only sequential reads of 'stdin' are supported)

//--- scanf-stdout.c
#include <stdio.h>
int main(void) {
  int x = 0;
  fscanf(stdout, "%d", &x);
  return x;
}
// SCANOUT: scanf-stdout.c:4:10: error: unsupported: fscanf on a FILE* stream other than stdin

//--- seek-stdin.c
#include <stdio.h>
int main(void) {
  fseek(stdin, 0, 0);
  return 0;
}
// SEEK: seek-stdin.c:3:3: error: unsupported: file positioning 'fseek' (FILE* streams are sequential-only)

//--- rewind-stdin.c
#include <stdio.h>
int main(void) {
  rewind(stdin);
  return 0;
}
// REWIND: rewind-stdin.c:3:3: error: unsupported: file positioning 'rewind' (FILE* streams are sequential-only)

//--- unget-stdin.c
#include <stdio.h>
int main(void) {
  ungetc('a', stdin);
  return 0;
}
// UNGET: unget-stdin.c:3:3: error: unsupported: call to 'ungetc' declared in a system header; not part of the supported C subset

//--- feof-stdin.c
#include <stdio.h>
int main(void) { return feof(stdin); }
// FEOF: feof-stdin.c:2:25: error: unsupported: call to 'feof' declared in a system header; not part of the supported C subset

//--- ferror-stdin.c
#include <stdio.h>
int main(void) { return ferror(stdin); }
// FERROR: ferror-stdin.c:2:25: error: unsupported: call to 'ferror' declared in a system header; not part of the supported C subset

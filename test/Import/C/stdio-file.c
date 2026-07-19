// C99-48 (c-testsuite 00187): FILE* stdio slice — import pins for the
// settled design. A `FILE *` local becomes an OWNED opaque handle over
// Rust std::fs: fopen(path, "w") creates/truncates a writable handle,
// fopen(path, "r") opens a readable one, fclose consumes the handle, and
// the SAME variable may be reassigned by a later fopen (serial reuse).
// fwrite/fread are byte-wise (element size 1), fgetc/getc return an i32
// byte or -1 at EOF, and fgets NUL-terminates and returns NULL at EOF.
//
// PIN FIRMNESS:
//   FIRM  - the whole file imports (importer exits 0, every function
//           below appears in the module);
//   FIRM  - the NULL-test of a fopen result (`if (!f) return ...;`)
//           imports as a conditional branch;
//   FIRM  - reassignment of the handle variable after fclose imports
//           (see byte_reader / line_reader);
//   FIRM  - EOF is the i32 constant -1 compared against the fgetc/getc
//           result;
//   LOOSE - the handle's MLIR type, the ops/opaque-call spellings that
//           produce and consume it, and any runtime helper names are
//           GREEN's choice; only block structure is pinned.
// RUN: emitrust-import-c %s | FileCheck %s

#include <stdio.h>

// fopen("w") with a NULL check, byte-wise fwrite, fclose.
int write_only(void) {
  char buf[3];
  buf[0] = 'q';
  buf[1] = '\n';
  buf[2] = '\0';
  FILE *f = fopen("import_stdio_file_scratch.txt", "w");
  if (!f)
    return 1;
  fwrite(buf, 1, 2, f);
  fclose(f);
  return 0;
}
// CHECK-LABEL: func.func @write_only
// The fopen NULL-test lowers to a real conditional.
// CHECK: cf.cond_br
// CHECK: return

// fgetc EOF loop, then fclose + REASSIGN the same handle variable for a
// getc EOF loop.
int byte_reader(void) {
  int total = 0;
  int c;
  FILE *f = fopen("import_stdio_file_scratch.txt", "r");
  if (!f)
    return -1;
  while ((c = fgetc(f)) != EOF)
    total += c;
  fclose(f);
  f = fopen("import_stdio_file_scratch.txt", "r");
  if (!f)
    return -1;
  while ((c = getc(f)) != EOF)
    total -= c;
  fclose(f);
  return total;
}
// CHECK-LABEL: func.func @byte_reader
// EOF materializes as i32 -1 and is compared against the read byte.
// CHECK-DAG: arith.constant -1 : i32
// CHECK: arith.cmpi
// CHECK: cf.cond_br
// CHECK: return

// Size-1 fread with its return count checked, then fclose + reassign for
// an fgets line loop (NULL-terminated iteration).
int line_reader(void) {
  char buf[7];
  int lines = 0;
  FILE *f = fopen("import_stdio_file_scratch.txt", "r");
  if (!f)
    return -1;
  if (fread(buf, 1, 6, f) != 6)
    lines = -1;
  fclose(f);
  f = fopen("import_stdio_file_scratch.txt", "r");
  while (fgets(buf, sizeof(buf), f) != NULL)
    lines++;
  fclose(f);
  return lines;
}
// CHECK-LABEL: func.func @line_reader
// CHECK: arith.cmpi
// CHECK: cf.cond_br
// CHECK: return

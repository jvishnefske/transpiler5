// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/range.c 2>&1 | FileCheck %s --check-prefix=RANGE
// RUN: not emitrust-import-c %t/range-dispatch.c 2>&1 | FileCheck %s --check-prefix=RANGED

// GNU case ranges are rejected at the case keyword by both switch
// lowerings: the structured one (top-level labels) and the dispatch one
// (labels nested inside inner statements, see switch-dispatch.c for the
// accepted shapes). Both diagnostics carry the file:line:col location.

// RANGE: range.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: GNU case range
// RANGED: range-dispatch.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: GNU case range

//--- range.c
int range(int x) {
  switch (x) {
  case 1 ... 3:
    return 1;
  default:
    return 0;
  }
}

//--- range-dispatch.c
int range_dispatch(int x) {
  int r = 0;
  switch (x) {
  case 0:
    while (x > 0) {
  case 1 ... 3:
      r = r + 1;
      x = x - 1;
    }
  }
  return r;
}

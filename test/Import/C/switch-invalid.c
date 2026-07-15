// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/duff.c 2>&1 | FileCheck %s --check-prefix=DUFF
// RUN: not emitrust-import-c %t/range.c 2>&1 | FileCheck %s --check-prefix=RANGE

// Case labels must sit at the top level of the switch body; a label buried
// inside a sub-statement (Duff's device) is rejected at the label, and GNU
// case ranges are rejected at the case keyword. Both diagnostics carry the
// file:line:col location.

// DUFF: duff.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: case label nested inside another statement
// RANGE: range.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: GNU case range

//--- duff.c
int duff(int x) {
  int r = 0;
  switch (x) {
  case 0:
    while (x > 0) {
  case 1:
      r = r + 1;
      x = x - 1;
    }
    break;
  }
  return r;
}

//--- range.c
int range(int x) {
  switch (x) {
  case 1 ... 3:
    return 1;
  default:
    return 0;
  }
}

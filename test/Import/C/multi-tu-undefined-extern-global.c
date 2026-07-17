// CTS-F2 boundary: an `extern` object that IS referenced must still be
// defined in some translation unit; the rejection is located at the use
// site (line 9, the load in main), not at the declaration.
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-empty.c 2>&1 | FileCheck %s

extern int x;

int main(void) {
  return x;
}

// CHECK: multi-tu-undefined-extern-global.c:9:{{[0-9]+}}: error: unsupported: extern global variable 'x' is referenced but not defined in any translation unit

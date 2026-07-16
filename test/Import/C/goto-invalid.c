// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// Computed goto (the GNU `goto *expr` extension) has no bounded static
// target set, so it is rejected with the file:line:col location of the
// goto. Plain gotos to named labels are supported (see goto.c).

int f(void) {
  goto *&&target;
target:
  return 0;
}

// CHECK: goto-invalid.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: computed goto

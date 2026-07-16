// FR-20: out-of-subset input fails with a located diagnostic and a nonzero
// exit code; no partial output is produced.
// RUN: not emitrust-cc --emit=rust %s -o - 2>&1 | FileCheck %s

int f(void) {
  goto *&&end; /* computed goto (GNU extension) is outside the subset */
end:
  return 0;
}

// The diagnostic carries the file:line:col location of the rejected goto.
// CHECK: reject.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: computed goto

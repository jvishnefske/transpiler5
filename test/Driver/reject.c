// FR-20: out-of-subset input fails with a located diagnostic and a nonzero
// exit code; no partial output is produced.
// RUN: not emitrust-cc --emit=rust %s -o - 2>&1 | FileCheck %s

int f(void) {
  int x = 0;
  goto end;
end:
  return x;
}

// The diagnostic carries the file:line:col location of the goto.
// CHECK: reject.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: goto statement

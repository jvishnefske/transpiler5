// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

int f(void) {
  int x = 0;
  goto end;
end:
  return x;
}

// The diagnostic carries the file:line:col location of the goto.
// CHECK: goto.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: goto statement

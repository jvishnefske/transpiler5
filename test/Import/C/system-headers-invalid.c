// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/fopen-call.c 2>&1 | FileCheck %s --check-prefix=FOPEN
// RUN: not emitrust-import-c %t/stdin-ref.c 2>&1 | FileCheck %s --check-prefix=STDIN
// RUN: not emitrust-import-c %t/addr-of.c 2>&1 | FileCheck %s --check-prefix=ADDROF
// RUN: not emitrust-import-c %t/main.c -isystem %t/sys 2>&1 | FileCheck %s --check-prefix=SYSVAR
// RUN: not emitrust-import-c %t/main.c -I %t/sys 2>&1 | FileCheck %s --check-prefix=PROJHDR
// RUN: emitrust-import-c %t/unref.c -isystem %t/sys | FileCheck %s --check-prefix=UNREF

// C99-39: system-header declarations are skipped at import time, so using
// one must be rejected at the use site with a located diagnostic naming the
// symbol. The same header on the `-I` path is a project header and keeps
// the eager whole-file import (its unsupported declaration is rejected even
// when unreferenced), while on `-isystem` only actual uses are rejected.

//--- fopen-call.c
#include <stdio.h>
int main(void) {
  fopen("a", "r");
  return 0;
}
// FOPEN: fopen-call.c:3:3: error: unsupported: call to 'fopen' declared in a system header; not part of the supported C subset

//--- stdin-ref.c
#include <stdio.h>
int main(void) {
  if (stdin) {
    return 1;
  }
  return 0;
}
// A FILE* use trips the pointer-expression rejections before the name
// lookup; the diagnostic is still located at the use site (the truth test
// of a data pointer is now the CTS-P8 null check, so the rejection names
// the unresolvable target instead).
// STDIN: stdin-ref.c:3:7: error: unsupported: pointer variable 'stdin' has no known target object

//--- addr-of.c
#include <stdlib.h>
int main(void) {
  int (*fp)(int) = abs;
  return fp(-3);
}
// ADDROF: addr-of.c:3:20: error: unsupported: taking the address of 'abs' declared in a system header; not part of the supported C subset

//--- sys/sysdecls.h
extern int sys_counter;
struct {
  int x;
} sys_anon;

//--- main.c
#include <sysdecls.h>
int main(void) {
  return sys_counter;
}
// SYSVAR: main.c:3:10: error: unsupported: reference to 'sys_counter' declared in a system header; not part of the supported C subset
// PROJHDR: sysdecls.h:{{[0-9]+}}:{{[0-9]+}}: error: unsupported
// PROJHDR-NOT: system header

//--- unref.c
#include <sysdecls.h>
int main(void) {
  return 0;
}
// The same unsupported header declarations, never referenced: the import
// succeeds and the module carries only the main-file symbol.
// UNREF-NOT: sys_anon
// UNREF: func.func @c_main
// UNREF-NOT: sys_anon

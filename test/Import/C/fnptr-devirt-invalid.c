// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/reassigned.c 2>&1 | FileCheck %s --check-prefix=REASSIGNED
// RUN: not emitrust-import-c %t/nonhosted.c 2>&1 | FileCheck %s --check-prefix=NONHOSTED
// RUN: not emitrust-import-c %t/stdout-arg.c 2>&1 | FileCheck %s --check-prefix=STDOUTARG
// RUN: not emitrust-import-c %t/stdout-store.c 2>&1 | FileCheck %s --check-prefix=STDOUTSTORE

// CTS-S (00189) rejections: static devirtualization only covers a global
// function pointer that is never reassigned anywhere in the TU and whose
// target is representable (an in-TU function, or a hosted variadic like
// fprintf routed to the printf machinery). Everything else keeps the
// existing located rejections, and `stdout` / FILE* values stay rejected
// everywhere EXCEPT the swallowed first-argument slot of a devirtualized
// fprintf call.

//--- reassigned.c
// A reassigned global fn-ptr cannot alias: it needs a real fn_ptr global,
// and a variadic fn_ptr type has no representation (existing wording,
// reused).
#include <stdio.h>
int (*fprintfptr)(FILE *, const char *, ...) = &fprintf;
void swap(void) { fprintfptr = 0; }
int main(void) {
  fprintfptr(stdout, "%d\n", 1);
  return 0;
}
// REASSIGNED: reassigned.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: variadic function pointer type

//--- nonhosted.c
// A never-reassigned fn-ptr to a NON-hosted external variadic cannot
// devirtualize (no printf routing exists for it) and falls back to the
// same variadic-fn-ptr rejection (existing wording, reused).
int external_log(const char *, ...);
int (*const logptr)(const char *, ...) = &external_log;
int main(void) {
  logptr("%d\n", 1);
  return 0;
}
// NONHOSTED: nonhosted.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: variadic function pointer type

//--- stdout-arg.c
// With a valid devirtualized fprintfptr call in the SAME TU, `stdout`
// passed to a user function (outside the swallowed slot) stays rejected
// at the use site (existing wording, reused).
#include <stdio.h>
int (*fprintfptr)(FILE *, const char *, ...) = &fprintf;
void take(FILE *f) { (void)f; }
int main(void) {
  fprintfptr(stdout, "%d\n", 1);
  take(stdout);
  return 0;
}
// STDOUTARG: stdout-arg.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer variable 'stdout' has no known target object

//--- stdout-store.c
// Storing `stdout` into a FILE* variable stays rejected (existing
// wording, reused).
#include <stdio.h>
int main(void) {
  FILE *g = stdout;
  (void)g;
  return 0;
}
// STDOUTSTORE: stdout-store.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: copying a global pointer variable

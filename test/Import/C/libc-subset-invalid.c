// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/exp-call.c 2>&1 | FileCheck %s --check-prefix=EXP
// RUN: not emitrust-import-c %t/log-call.c 2>&1 | FileCheck %s --check-prefix=LOG
// RUN: not emitrust-import-c %t/pow-call.c 2>&1 | FileCheck %s --check-prefix=POW
// RUN: not emitrust-import-c %t/rand-call.c 2>&1 | FileCheck %s --check-prefix=RAND
// RUN: not emitrust-import-c %t/strtok-call.c 2>&1 | FileCheck %s --check-prefix=STRTOK
// RUN: not emitrust-import-c %t/memmove-value.c 2>&1 | FileCheck %s --check-prefix=MEMMOVEVALUE
// RUN: not emitrust-import-c %t/atoi-scalar.c 2>&1 | FileCheck %s --check-prefix=ATOISCALAR

// C99-48 boundaries of the curated libc subset: functions outside the
// curation keep located rejections, and the curated ones reject shapes
// outside the region/statement model.

// exp/log/pow are rejected by policy, not omission: C imposes no accuracy
// requirement on them, libm implementations disagree in the last bits,
// and rustc may constant-fold through a different libm than the C
// reference — no bit-exact safe-Rust mapping can be argued (contrast the
// IEEE-exact fabs/sqrt/floor/ceil, which every conforming implementation
// computes identically).
//--- exp-call.c
#include <math.h>
int main(void) {
  double x = exp(1.0);
  return x > 2.0;
}
// EXP: exp-call.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: 'exp' has no bit-exact Rust mapping

//--- log-call.c
#include <math.h>
int main(void) {
  double x = log(2.0);
  return x > 0.0;
}
// LOG: log-call.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: 'log' has no bit-exact Rust mapping

//--- pow-call.c
#include <math.h>
int main(void) {
  double x = pow(2.0, 10.0);
  return x > 1000.0;
}
// POW: pow-call.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: 'pow' has no bit-exact Rust mapping

// An uncurated <stdlib.h> function keeps the system-header use-site
// rejection.
//--- rand-call.c
#include <stdlib.h>
int main(void) {
  return rand();
}
// RAND: rand-call.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to 'rand' declared in a system header; not part of the supported C subset

// So does an uncurated <string.h> function.
//--- strtok-call.c
#include <string.h>
int main(void) {
  char a[8] = "a,b";
  strtok(a, ",");
  return 0;
}
// STRTOK: strtok-call.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to 'strtok' declared in a system header; not part of the supported C subset

// memmove's char* result has no decomposed representation; like memcpy it
// is statement-position only. (The K&R-style int declaration reaches the
// call check, as in the memcpy negative.)
//--- memmove-value.c
int memmove(char *, char *, int);
int main(void) {
  char a[8];
  char b[8];
  b[0] = 0;
  int r = memmove(a, b, 1);
  return r;
}
// MEMMOVEVALUE: memmove-value.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: memmove return value must be unused

// atoi requires a char region (a char array, a literal backing, or a
// pointer into either); the address of a scalar is rejected.
//--- atoi-scalar.c
#include <stdlib.h>
int main(void) {
  char c = '7';
  return atoi(&c);
}
// ATOISCALAR: atoi-scalar.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the address of a scalar object is not a string region

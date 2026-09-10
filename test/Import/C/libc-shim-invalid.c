// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/expf.c 2>&1 | FileCheck %s --check-prefix=EXPF
// RUN: not emitrust-import-c %t/logf.c 2>&1 | FileCheck %s --check-prefix=LOGF
// RUN: not emitrust-import-c %t/powf.c 2>&1 | FileCheck %s --check-prefix=POWF
// RUN: not emitrust-import-c %t/fputs-stderr.c 2>&1 | FileCheck %s --check-prefix=STDERR
// RUN: not emitrust-import-c %t/fputs-value.c 2>&1 | FileCheck %s --check-prefix=FPUTSVALUE
// RUN: not emitrust-import-c %t/locale-native.c 2>&1 | FileCheck %s --check-prefix=NATIVE
// RUN: not emitrust-import-c %t/locale-value.c 2>&1 | FileCheck %s --check-prefix=LOCALEVALUE
// RUN: not emitrust-import-c %t/abort-value.c 2>&1 | FileCheck %s --check-prefix=ABORTVALUE
// RUN: not emitrust-import-c %t/abort-stmt.c 2>&1 | FileCheck %s --check-prefix=ABORTSTMT
// RUN: not emitrust-import-c %t/strcspn-scalar.c 2>&1 | FileCheck %s --check-prefix=CSPNSCALAR
// RUN: not emitrust-import-c %t/sqrtl.c 2>&1 | FileCheck %s --check-prefix=SQRTL
// RUN: emitrust-cc --emit=import --recover %t/ledger.c -o - 2>&1 | FileCheck %s --check-prefix=LEDGER

// FR-224 BOUNDARIES of the shim table. Every shape below is a LOCATED
// rejection on purpose, and the reason differs per shape -- which is why
// each gets its own wording rather than the generic system-header one.
// The bar for this wave is "make it work; a located rejection is an
// acceptable floor; silently wrong is not", so these are the floor.

// expf/logf/powf are the SAME policy rejection as exp/log/pow, not an
// omission. The `f` suffix narrows the width, not the disagreement: C
// imposes no accuracy requirement on these, glibc's `expf` and Rust's
// `f32::exp` are different implementations of an unmandated function,
// and rustc may constant-fold through a third. Contrast sqrtf/fabsf/
// floorf/ceilf, which IEEE-754 mandates exactly and which ARE admitted.
// This costs the corpus one case (gaussian_kernel_lib) and that is the
// correct price.
//--- expf.c
#include <math.h>
int main(void) {
  float x = expf(1.0f);
  return x > 2.0f;
}
// EXPF: expf.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: 'expf' has no bit-exact Rust mapping

//--- logf.c
#include <math.h>
int main(void) {
  float x = logf(2.0f);
  return x > 0.0f;
}
// LOGF: logf.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: 'logf' has no bit-exact Rust mapping

//--- powf.c
#include <math.h>
int main(void) {
  float x = powf(2.0f, 3.0f);
  return x > 7.0f;
}
// POWF: powf.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: 'powf' has no bit-exact Rust mapping

// The fputs stream slot is the same devirtualized-stdout frontier the
// fprintf swallow draws: only the literal `stdout`. `stderr` is a
// DIFFERENT stream, and the differential oracle compares stdout -- a
// silent redirection of stderr bytes into stdout would pass every test
// this project runs and still be wrong.
//--- fputs-stderr.c
#include <stdio.h>
int main(void) {
  fputs("oops\n", stderr);
  return 0;
}
// STDERR: fputs-stderr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: fputs to a FILE* stream (only the literal 'stdout' is supported)

// fputs' int result (a nonnegative value, or EOF) has no representation
// in the statement-position lowering, so a value use keeps the
// system-header rejection rather than silently yielding a made-up
// number.
//--- fputs-value.c
#include <stdio.h>
int main(void) {
  return fputs("x", stdout) < 0;
}
// FPUTSVALUE: fputs-value.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to 'fputs' declared in a system header

// `setlocale(LC_ALL, "")` selects the IMPLEMENTATION-DEFINED native
// environment locale. That is not a no-op: under a UTF-8 or Latin-1
// locale it is exactly what makes `isalpha` accept accented bytes. Only
// the literal "C" -- which is already the startup state (C11 7.11.1.1p4)
// -- is observationally empty, so only "C" is admitted.
//--- locale-native.c
#include <locale.h>
int main(void) {
  setlocale(LC_ALL, "");
  return 0;
}
// NATIVE: locale-native.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: setlocale to a locale other than the literal "C"

// setlocale's `char *` result names the locale in effect; the elision
// has no value to give back, so a use of it rejects rather than
// inventing one.
//--- locale-value.c
#include <locale.h>
#include <string.h>
int main(void) {
  return strlen(setlocale(LC_ALL, "C")) > 0;
}
// LOCALEVALUE: locale-value.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported

// FR-224's MEASURED NO-GO, and the most useful thing this spike found.
// `std::process::abort()` looked exact -- same SIGABRT, same absence of
// destructors and of a flush, and the exit status DOES agree (134 both
// sides). The byte-diff oracle refuted it anyway:
//
//     printf("before abort\n"); abort();
//
// clang+glibc with stdout redirected to a file writes NOTHING, because
// C's stdout is FULLY buffered off a terminal and abort does not flush
// it (C11 7.22.4.1p2, implementation-defined, glibc declines). The
// emitted crate writes `before abort`, because Rust's Stdout is a
// LineWriter that flushed on the newline. Measured diff: `0a1 > before
// abort`.
//
// The defect is therefore the emitted crate's BUFFERING MODEL, not
// abort: a normal return/exit flushes on both sides and agrees byte for
// byte even for a partial line (measured separately). abort is just the
// only admitted construct that can observe the difference, and nothing
// local to the call repairs it -- flushing here writes MORE than glibc,
// not less. So it refuses, and the wording says why, so that whoever
// picks up the fully-buffered-stdout FR finds this note.
//--- abort-stmt.c
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  printf("before abort\n");
  abort();
  return 0;
}
// ABORTSTMT: abort-stmt.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: 'abort' terminates without flushing C's fully buffered stdout, but the emitted crate's line-buffered stdout has already written every completed line

// abort returns void in every conforming declaration, so a value use can
// only come from a redeclaration; it rejects rather than silently
// dropping a call that terminates the process.
//--- abort-value.c
int abort(void);
int main(void) {
  return abort();
}
// ABORTVALUE: abort-value.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: abort return value must be unused

// strcspn keeps strcmp's region rule: its arguments must designate char
// arrays. The address of a scalar is not a string region, and refusing
// here is what keeps the helper from being handed a one-element slice
// with no NUL in it.
//--- strcspn-scalar.c
#include <string.h>
int main(void) {
  char c = 'x';
  return (int)strcspn(&c, "x");
}
// CSPNSCALAR: strcspn-scalar.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported

// The LONG DOUBLE forms are not admitted at any name: Rust has no
// f80/f128 to lower them to, and the prototype gate (which matches
// `double f(double)` or `float f(float)` EXACTLY) is what keeps them
// out. This pins that the FR-224 widening did not accidentally admit a
// third width.
//--- sqrtl.c
#include <math.h>
int main(void) {
  long double x = sqrtl(2.0L);
  return x > 1.0L;
}
// SQRTL: sqrtl.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported

// FR-224 COUNTING HYGIENE. Both of this wave's NO-GO wordings previously
// tabulated as `libc:expf` / `libc:abort`, because they went through the
// generic system-header rejection; giving them a wording that says WHY
// would otherwise have dropped them into `other`, the census's largest
// junk bucket, and a reader ranking work by that census would then have
// seen nothing at all where two DELIBERATE refusals live. Each gets its
// own ledger tag so "refused on purpose" never reads as "missing work".
// The `libm-not-bit-exact` tag also picks up the pre-existing f64
// pow/exp/log, which had been in `other` since C99-48.
//--- ledger.c
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
void uses_abort(int n) { if (n < 0) abort(); printf("%d\n", n); }
float uses_expf(float x) { return expf(x); }
double uses_pow(double x) { return pow(x, 2.0); }
int fine(int a) { return a * 2; }
// LEDGER: stubbed 'uses_abort' [abort-stdout-flush]
// LEDGER: stubbed 'uses_expf' [libm-not-bit-exact]
// LEDGER: stubbed 'uses_pow' [libm-not-bit-exact]
// LEDGER: blocker tabulation
// LEDGER-DAG: abort-stdout-flush 1
// LEDGER-DAG: libm-not-bit-exact 2

// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/const-dest.c 2>&1 | FileCheck %s --check-prefix=CONSTDEST
// RUN: not emitrust-import-c %t/same-param.c 2>&1 | FileCheck %s --check-prefix=SAMEPARAM
// RUN: not emitrust-import-c %t/mixed-elem.c 2>&1 | FileCheck %s --check-prefix=MIXED
// RUN: not emitrust-import-c %t/mixed-literal.c 2>&1 | FileCheck %s --check-prefix=MIXEDLIT
// RUN: not emitrust-import-c %t/u8-strlen.c 2>&1 | FileCheck %s --check-prefix=U8STRLEN
// RUN: not emitrust-import-c %t/u8-strcpy.c 2>&1 | FileCheck %s --check-prefix=U8STRCPY

// FR-72 frontier: byte-slice PARAMETERS as string-function regions cover
// EXACTLY the shapes the helpers can render as safe, element-agreeing
// Rust. Everything else keeps a located rejection — rejection is a
// feature, and none of these shapes may silently emit code that only
// fails (or worse, differs) downstream: a shared (const) parameter as a
// mutable destination would be rustc E0596 only after emission; a
// same-parameter memcpy src/dst pair is C UB the distinct-param
// convention cannot certify (arrays keep their provable-same-base
// `copy_within` refinement — parameters have no provable extent); a
// MIXED i8/ui8 call has no helper signature both regions satisfy; and
// the str*-family/strchr/atoi helpers are i8-typed, so a ui8 region
// there would be rustc E0308 after emission. Every callee is called with
// TWO distinct arrays so the FR-40 owner lift stays out of the way and
// the pinned wording is the slice-PARAMETER path's.

// A shared byte-slice parameter (`const uint8_t *`, borrowed `&[u8]`)
// can only be a SOURCE region; as the memset destination it rejects at
// import instead of failing borrowck in the emitted crate.
// CONSTDEST: const-dest.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a shared byte-slice parameter cannot be a mutable string argument

//--- const-dest.c
#include <string.h>
typedef unsigned char uint8_t;
void wipe(const uint8_t *d, unsigned n) {
  memset(d, 0, n);
}
int main(void) {
  uint8_t b[4];
  uint8_t c[4];
  wipe(b, 4u);
  wipe(c, 4u);
  return 0;
}

// memcpy with source and destination in the SAME parameter: overlapping
// memcpy is C UB, and unlike two array cursors the parameter's extent is
// not provable, so the array-only copy_within refinement does not apply.
// SAMEPARAM: same-param.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: memcpy source and destination point into the same slice parameter 'p'

//--- same-param.c
#include <string.h>
typedef unsigned char uint8_t;
void shift(uint8_t *p, unsigned n) {
  memcpy(p, p + 1, n);
}
int main(void) {
  uint8_t b[4];
  uint8_t c[4];
  shift(b, 2u);
  shift(c, 2u);
  return 0;
}

// A MIXED-element call (ui8 destination, i8 source): no helper signature
// fits both regions, so the call rejects at import instead of E0308 in
// the emitted crate.
// MIXED: mixed-elem.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: memcpy arguments mix char and unsigned char regions

//--- mixed-elem.c
#include <string.h>
typedef unsigned char uint8_t;
void mix(uint8_t *d, const char *s, unsigned n) {
  memcpy(d, s, n);
}
int main(void) {
  uint8_t b[4];
  uint8_t c[4];
  mix(b, "ab", 2u);
  mix(c, "cd", 2u);
  return 0;
}

// The same mixed rule when the i8 region is a string LITERAL backing
// (literal backings are i8 by the C99-28 model).
// MIXEDLIT: mixed-literal.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: memcmp arguments mix char and unsigned char regions

//--- mixed-literal.c
#include <string.h>
typedef unsigned char uint8_t;
int is_magic(const uint8_t *d) {
  return memcmp(d, "ab", 2) == 0;
}
int main(void) {
  uint8_t b[2];
  uint8_t c[2];
  b[0] = 'a';
  c[0] = 'b';
  return is_magic(b) + is_magic(c);
}

// The str*-family (and strchr/atoi/strlen) helpers stay i8-typed: a ui8
// region argument keeps a located rejection.
// U8STRLEN: u8-strlen.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: string function argument over an unsigned char region

//--- u8-strlen.c
#include <string.h>
typedef unsigned char uint8_t;
int len_of(const uint8_t *s) {
  return (int)strlen(s);
}
int main(void) {
  uint8_t b[4];
  uint8_t c[4];
  b[0] = 0;
  c[0] = 0;
  return len_of(b) + len_of(c);
}

// Statement-position spelling of the same i8-only rule (strcpy).
// U8STRCPY: u8-strcpy.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: string function argument over an unsigned char region

//--- u8-strcpy.c
#include <string.h>
typedef unsigned char uint8_t;
void put(uint8_t *d) {
  strcpy(d, "x");
}
int main(void) {
  uint8_t b[4];
  uint8_t c[4];
  put(b);
  put(c);
  return 0;
}

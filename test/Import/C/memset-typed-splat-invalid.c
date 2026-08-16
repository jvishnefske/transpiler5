// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/i16-count.c 2>&1 | FileCheck %s --check-prefix=I16COUNT
// RUN: not emitrust-import-c %t/i16-odd.c 2>&1 | FileCheck %s --check-prefix=I16ODD
// RUN: not emitrust-import-c %t/i16-fill.c 2>&1 | FileCheck %s --check-prefix=I16FILL
// RUN: not emitrust-import-c %t/member-i16-fill.c 2>&1 | FileCheck %s --check-prefix=MEMFILL
// RUN: not emitrust-import-c %t/u32-local-fill.c 2>&1 | FileCheck %s --check-prefix=U32LOCALFILL
// RUN: not emitrust-import-c %t/i16-offset.c 2>&1 | FileCheck %s --check-prefix=I16OFFSET
// RUN: not emitrust-import-c %t/i16-memcpy.c 2>&1 | FileCheck %s --check-prefix=I16MEMCPY
// RUN: not emitrust-import-c %t/i16-memcmp.c 2>&1 | FileCheck %s --check-prefix=I16MEMCMP
// RUN: not emitrust-import-c %t/i16-local-memcpy.c 2>&1 | FileCheck %s --check-prefix=I16LOCALCPY
// RUN: not emitrust-import-c %t/float-local.c 2>&1 | FileCheck %s --check-prefix=FLOAT

// FR-97 frontier: the byte-splat memset image over typed integer
// arrays admits EXACTLY the gated subset — a compile-time-constant
// byte count that is a multiple of the element size, and a
// compile-time-constant fill byte (whose per-width replication is then
// exact for EVERY byte value). Everything else keeps a LOCATED
// rejection — rejection is a feature, and none of these shapes may
// silently emit wrong code. A non-constant (or non-multiple) count
// cannot prove whole-element coverage; a non-constant fill byte has no
// compile-time replicated word; typed regions outside the memset
// DESTINATION position (memcpy/memmove/memcmp, either side) have no
// admitted image at all; an OFFSET local destination (`a + 1`) is
// unexercised by the motivating corpus and stays on the historical
// char-array rejection this wave, as does every unmapped element type
// (float). The FR-87 u32 member wordings ("an unsigned int region")
// stay pinned verbatim in string-fn-member-region-invalid.c; the new
// widths share the generalized "a typed integer region" spelling, and
// the u32 spelling now also names LOCAL unsigned int destinations.
// Every wording below is pinned verbatim as measured against the
// built tool.

// A runtime byte count over an i16 local array: whole-element coverage
// is not provable, so the call rejects at import.
// I16COUNT: i16-count.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: memset over a typed integer region requires a constant byte count that is a multiple of 2

//--- i16-count.c
#include <string.h>
#include <stdint.h>
int16_t f(int n) {
  int16_t a[8];
  memset(a, 0xFF, (unsigned long)n * 2u);
  return a[0];
}
int main(void) { return (int)(f(8) & 1); }

// A constant count that is NOT a multiple of 2 would split an element:
// no whole-element fill image covers it.
// I16ODD: i16-odd.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: memset over a typed integer region requires a constant byte count that is a multiple of 2

//--- i16-odd.c
#include <string.h>
#include <stdint.h>
int16_t f(void) {
  int16_t a[8];
  memset(a, 0x00, 7);
  return a[0];
}
int main(void) { return (int)(f() & 1); }

// A runtime fill byte has no compile-time replicated word.
// I16FILL: i16-fill.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: memset over a typed integer region requires a constant fill byte

//--- i16-fill.c
#include <string.h>
#include <stdint.h>
int16_t f(int c) {
  int16_t a[8];
  memset(a, c, sizeof(a));
  return a[0];
}
int main(void) { return (int)(f(3) & 1); }

// The same fill gate guards the MEMBER destinations FR-87 opened.
// MEMFILL: member-i16-fill.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: memset over a typed integer region requires a constant fill byte

//--- member-i16-fill.c
#include <string.h>
#include <stdint.h>
struct S { int16_t last[8]; unsigned int n; };
void f(struct S *s, int c) {
  memset(s->last, c, sizeof(s->last));
}
int main(void) {
  struct S s;
  f(&s, 3);
  return (int)(s.last[0] & 1);
}

// A LOCAL unsigned int destination keeps FR-87's u32 wording verbatim:
// the diagnostic spelling is keyed by the element, not the root.
// U32LOCALFILL: u32-local-fill.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: memset over an unsigned int region requires a constant fill byte

//--- u32-local-fill.c
#include <string.h>
unsigned int f(int c) {
  unsigned int a[8];
  memset(a, c, sizeof(a));
  return a[0];
}
int main(void) { return (int)(f(3) & 1u); }

// An OFFSET local destination (nonzero cursor) is outside this wave's
// gate and keeps the historical char-array rejection.
// I16OFFSET: i16-offset.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: string function argument must designate a char array

//--- i16-offset.c
#include <string.h>
#include <stdint.h>
int16_t f(void) {
  int16_t a[8];
  memset(a + 1, 0xFF, 14);
  return a[1];
}
int main(void) { return (int)(f() & 1); }

// Typed member regions are memset-destination-only: memcpy has no
// admitted image (the same wall FR-87 pinned for u32, generalized).
// I16MEMCPY: i16-memcpy.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: string function argument over a typed integer region

//--- i16-memcpy.c
#include <string.h>
#include <stdint.h>
struct S { int16_t a[8]; int16_t b[8]; unsigned int n; };
void f(struct S *s) {
  memcpy(s->a, s->b, 16);
}
int main(void) {
  struct S s;
  s.b[0] = 5;
  f(&s);
  return (int)(s.a[0] & 1);
}

// ... and neither has memcmp (value position).
// I16MEMCMP: i16-memcmp.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: string function argument over a typed integer region

//--- i16-memcmp.c
#include <string.h>
#include <stdint.h>
struct S { int16_t a[8]; int16_t b[8]; unsigned int n; };
int f(struct S *s) {
  return memcmp(s->a, s->b, 16);
}
int main(void) {
  struct S s;
  s.a[0] = 1;
  s.b[0] = 2;
  return f(&s) & 1;
}

// A LOCAL typed array in a memcpy position never enters the member
// channel and keeps the historical char-array rejection.
// I16LOCALCPY: i16-local-memcpy.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: string function argument must designate a char array

//--- i16-local-memcpy.c
#include <string.h>
#include <stdint.h>
int16_t f(void) {
  int16_t a[8];
  int16_t b[8];
  b[0] = 5;
  memcpy(a, b, 16);
  return a[0];
}
int main(void) { return (int)(f() & 1); }

// An element outside the map (float) stays on the char-array wall:
// byte replication has no float image.
// FLOAT: float-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: string function argument must designate a char array

//--- float-local.c
#include <string.h>
float f(void) {
  float a[8];
  memset(a, 0x00, sizeof(a));
  return a[0];
}
int main(void) { return (int)f(); }

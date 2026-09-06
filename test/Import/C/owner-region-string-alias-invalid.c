// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/strcpy.c 2>&1 | FileCheck %s --check-prefix=STRCPY
// RUN: not emitrust-import-c %t/strncpy.c 2>&1 | FileCheck %s --check-prefix=STRNCPY
// RUN: not emitrust-import-c %t/strcat.c 2>&1 | FileCheck %s --check-prefix=STRCAT
// RUN: not emitrust-import-c %t/unpromoted.c 2>&1 | FileCheck %s --check-prefix=UNPROMOTED
// RUN: not emitrust-import-c %t/same-param.c 2>&1 | FileCheck %s --check-prefix=SAMEPARAM

// The FRONTIER of the owner-region same-place copy: which two-cursor shapes
// inside a promoted owner method are REFUSED, and with what wording.
//
// Inside a Phase-4 owner method every data-pointer parameter is an i64 index
// into the SINGLE receiver region `self.data`, so a mutable destination and a
// shared source built from two parameters borrow ONE place twice -- rustc
// E0502 in a crate emitrust-cc had already exited 0 over. The byte-family
// copies (memcpy/memmove) have a same-region image for exactly this, the
// `copy_within` helper, and take it (see owner-region-string-alias.c). The
// str*-family COPIES do not: `__emitrust_strcpy`/`strncpy`/`strcat` are
// two-slice helpers whose length is discovered from the source's NUL, and no
// same-region image of them exists in the tree. Inventing one is a separate
// increment; until it exists these shapes reject LOUDLY and located rather
// than emitting a crate that does not build.
//
// The rejection names the OWNER ARRAY, not a parameter: the parameters are
// cursors, the region is the owner's array, and naming `dst` would point the
// reader at the wrong object. The comparison members of the family
// (strcmp/strncmp/memcmp) are NOT here on purpose -- they take two SHARED
// borrows, which is legal Rust, and keep working.

// STRCPY: strcpy.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: strcpy source and destination point into the same owner region 'a'

//--- strcpy.c
#include <string.h>

static void scpy(char *dst, const char *src) { strcpy(dst, src); }

int main(void) {
  char a[16];
  memset(a, 0, 16);
  a[0] = 'a';
  a[1] = 0;
  scpy(&a[8], &a[0]);
  return a[8];
}

// STRNCPY: strncpy.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: strncpy source and destination point into the same owner region 'a'

//--- strncpy.c
#include <string.h>

static void sncpy(char *dst, const char *src, int n) {
  strncpy(dst, src, (size_t)n);
}

int main(void) {
  char a[16];
  memset(a, 0, 16);
  a[0] = 'a';
  a[1] = 0;
  sncpy(&a[8], &a[0], 4);
  return a[8];
}

// STRCAT: strcat.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: strcat source and destination point into the same owner region 'a'

//--- strcat.c
#include <string.h>

static void scat(char *dst, const char *src) { strcat(dst, src); }

int main(void) {
  char a[16];
  memset(a, 0, 16);
  a[0] = 'a';
  a[1] = 0;
  a[8] = 'x';
  a[9] = 0;
  scat(&a[8], &a[0]);
  return a[8];
}

// A class that does NOT promote keeps the historical CALL-SITE aliasing
// rejection verbatim: the array is above the 32-element owner-promotion
// limit, so the two arguments stay real borrows of one object and collide on
// the (root, field-path) key before any callee body is consulted. Nothing
// about the owner-region image loosens this.
// UNPROMOTED: unpromoted.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 'a')
// UNPROMOTED-NOT: owner region

//--- unpromoted.c
#include <string.h>

static void cpy(char *dst, const char *src, int n) {
  memcpy(dst, src, (size_t)n);
}

int main(void) {
  char a[64];
  memset(a, 0, 64);
  cpy(&a[4], &a[0], 4);
  return a[4];
}

// Two cursors out of ONE parameter keep FR-72's rejection verbatim, INSIDE a
// promoted owner method as much as outside it. The owner-region image
// deliberately does not reach this shape: it is keyed on two DISTINCT
// declarations that `planOwners` proves share a region, and admitting a
// single parameter is a separate decision with its own pinned wording. This
// case exists so a later refactor of the branch above cannot quietly loosen
// it.
// SAMEPARAM: same-param.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: memmove source and destination point into the same slice parameter 'p'
// SAMEPARAM-NOT: owner region

//--- same-param.c
#include <string.h>

static void sh(char *p, int n) { memmove(p + 2, p, (size_t)n); }

int main(void) {
  char a[8];
  memset(a, 0, 8);
  sh(a, 4);
  return a[2];
}

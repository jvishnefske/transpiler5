// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/unguarded.c 2>&1 | FileCheck %s --check-prefix=UNGUARDED
// RUN: not emitrust-import-c %t/arith-outside.c 2>&1 | FileCheck %s --check-prefix=ARITH
// RUN: not emitrust-import-c %t/reassign.c 2>&1 | FileCheck %s --check-prefix=REASSIGN
// RUN: not emitrust-import-c %t/mutable.c 2>&1 | FileCheck %s --check-prefix=MUTABLE
// RUN: not emitrust-import-c %t/char-elem.c 2>&1 | FileCheck %s --check-prefix=CHARELEM
// RUN: not emitrust-import-c --externals-trait %t/decl-only.c %t/decl-only-other.c 2>&1 | FileCheck %s --check-prefix=DECLONLY

// FR-88 frontier: the NULLABLE (Option-wrapped) byte-slice class covers
// EXACTLY the conservative guard-dominance shapes — a `const uint8_t *`
// parameter whose every region use is a byte-family SOURCE under a
// proven null guard. Everything else DECLINES to the historical
// classification, and the decline is what keeps the model sound: a
// declined parameter keeps the statically-non-null fold, so its
// null-constant call sites MUST keep their located rejection verbatim —
// otherwise the folded guard would run the region code against a null
// that was never representable. Each arm here is a decline shape plus
// the None-shaped call site that would have been legal had the
// parameter qualified; the pinned wording is the call-site rejection's.

// A region use OUTSIDE every null guard: the guard cannot dominate a use
// that precedes it, so the parameter declines and the null call site
// rejects.
// UNGUARDED: unguarded.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null pointer constant in a pointer expression

//--- unguarded.c
#include <string.h>
typedef unsigned char uint8_t;
int f(const uint8_t *p, unsigned int n) {
  uint8_t buf[8];
  memcpy(buf, p, 8);
  if (p != 0) {
    n += 1u;
  }
  return (int)buf[0] + (int)n;
}
int main(void) {
  uint8_t a[8];
  uint8_t b[8];
  return f(a, 1u) + f(b, 2u) + f(0, 3u);
}

// Pointer ARITHMETIC on the nullable candidate outside the guard (a
// walking cursor is a slice-parameter shape, not an Option shape).
// ARITH: arith-outside.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null pointer constant in a pointer expression

//--- arith-outside.c
#include <string.h>
typedef unsigned char uint8_t;
int g(const uint8_t *p, unsigned int n) {
  uint8_t buf[4];
  if (p != 0) {
    memcpy(buf, p, 4);
  }
  return (int)*(p + 1) + (int)n;
}
int main(void) {
  uint8_t a[8];
  uint8_t b[8];
  return g(a, 1u) + g(b, 2u) + g(0, 3u);
}

// REASSIGNMENT of the parameter: the pointer acts as a mutable cursor
// variable, which the Option value model cannot represent. The declined
// parameter falls back to the slice class, whose own rebinding rule
// rejects FIRST (in the body, before any call site is reached) — the
// measured decline wording.
// REASSIGN: reassign.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assignment would rebind to a different object

//--- reassign.c
#include <string.h>
typedef unsigned char uint8_t;
int h(const uint8_t *p, unsigned int n) {
  uint8_t buf[4];
  if (p != 0) {
    memcpy(buf, p, 4);
    p = buf;
  }
  return (int)n;
}
int main(void) {
  uint8_t a[8];
  uint8_t b[8];
  return h(a, 1u) + h(b, 2u) + h(0, 3u);
}

// A MUTABLE (non-const) pointee never qualifies this wave:
// `Option<&mut [u8]>` is not Copy, so the per-use-site unwrap model
// would move it. The guarded WRITE shape declines and the null call
// site keeps its rejection.
// MUTABLE: mutable.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null pointer constant in a pointer expression

//--- mutable.c
#include <string.h>
typedef unsigned char uint8_t;
void w(uint8_t *p, unsigned int n) {
  uint8_t src[4] = {1, 2, 3, 4};
  if (p != 0) {
    memcpy(p, src, 4);
  }
  (void)n;
}
int main(void) {
  uint8_t a[8];
  uint8_t b[8];
  w(a, 1u);
  w(b, 2u);
  w(0, 3u);
  return 0;
}

// A `const char *` (i8) parameter stays outside the class — the
// convention is the u8 byte-region domain (`Option<&[u8]>`); the same
// guarded-memcpy shape over char declines and the null call site keeps
// its rejection.
// CHARELEM: char-elem.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null pointer constant in a pointer expression

//--- char-elem.c
#include <string.h>
void s(const char *p, unsigned int len) {
  char buf[16] = {0};
  unsigned int n = len > 16u ? 16u : len;
  if (0 != p) {
    memcpy(buf, p, n);
  }
}
int main(void) {
  char a[4];
  char b[4];
  s(a, 4u);
  s(b, 4u);
  s(0, 0u);
  return 0;
}

// FR-75 interplay: a DECLARATION-ONLY callee cannot be classified (no
// body to scan), so its requirement signature stays the plain FR-75
// slice — it does NOT grow `Option<&[u8]>` by call-site consensus
// (wrapping by consensus risks multi-TU signature divergence between a
// defining TU and a declaration-only TU) — and a null-constant argument
// to it keeps the located rejection.
// DECLONLY: decl-only.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null pointer constant in a pointer expression

//--- decl-only.c
typedef unsigned char uint8_t;
void sink_decl(const uint8_t *p, unsigned int n);
void caller(void) {
  sink_decl(0, 0u);
}

//--- decl-only-other.c
typedef unsigned char uint8_t;
void sink_decl(const uint8_t *p, unsigned int n);
void other_caller(void) {
  uint8_t buf[4];
  sink_decl(buf, 4u);
}

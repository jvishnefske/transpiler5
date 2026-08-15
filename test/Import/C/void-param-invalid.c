// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/nonbyte.c 2>&1 | FileCheck %s --check-prefix=NONBYTE
// RUN: not emitrust-import-c %t/mixed.c 2>&1 | FileCheck %s --check-prefix=MIXED
// RUN: not emitrust-import-c %t/ret.c 2>&1 | FileCheck %s --check-prefix=RET
// RUN: not emitrust-import-c %t/cmp.c 2>&1 | FileCheck %s --check-prefix=CMP
// RUN: not emitrust-import-c %t/truthmix.c 2>&1 | FileCheck %s --check-prefix=TRUTHMIX
// RUN: not emitrust-import-c %t/declonly.c 2>&1 | FileCheck %s --check-prefix=DECLONLY
// RUN: not emitrust-import-c %t/storeglobal.c 2>&1 | FileCheck %s --check-prefix=STOREGLOBAL
// RUN: not emitrust-import-c %t/field.c 2>&1 | FileCheck %s --check-prefix=FIELD
// RUN: not emitrust-import-c %t/callsite-int.c 2>&1 | FileCheck %s --check-prefix=CALLINT
// RUN: not emitrust-import-c %t/callsite-struct.c 2>&1 | FileCheck %s --check-prefix=CALLSTRUCT
// RUN: not emitrust-import-c %t/probe.cpp 2>&1 | FileCheck %s --check-prefix=CPP

// FR-71 frontier: the byte-cursor admission for `void *` parameters
// (void-param-byte.c) covers EXACTLY the params whose every body use
// converts to one consistent byte pointee. Everything else keeps its
// verbatim located rejection — a conversion to a non-byte pointee, mixed
// byte pointees, returning the void*, comparing it against another
// pointer, mixing a truth test with a byte conversion, a declaration-only
// signature (the concession is a property of the DEFINITION's body),
// storing the void* into a global, a void* in a composite position, the
// C++ path (the admission is C-only; C++ diverts before it), and — per
// function, never a silent cast — a call-site argument that is not an
// admissible byte view. Rejection is a feature: no shape here may
// silently emit wrong code.

// A conversion to a non-byte pointee (unsigned *) does not qualify.
// NONBYTE: nonbyte.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: void pointer parameter

//--- nonbyte.c
int nonbyte(void *p) { unsigned *w = p; return (int)w[0]; }

// Two DIFFERENT byte pointees (uint8_t* and char*) in one body: the
// element would be ambiguous, so the scan declines.
// MIXED: mixed.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: void pointer parameter

//--- mixed.c
typedef unsigned char uint8_t;
int mixed(void *p, unsigned n) {
  uint8_t *a = p;
  char *b = p;
  return a[0] + b[n - 1];
}

// Returning the void* is a use no byte conversion consumes (and void*
// RESULTS are outside FR-71 entirely).
// RET: ret.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: void pointer parameter

//--- ret.c
void *ret_it(void *p) { return p; }

// Pointer comparison does not qualify.
// CMP: cmp.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: void pointer parameter

//--- cmp.c
int cmp_ptr(const void *a, const void *b) { return a == b; }

// A truth test mixed with a byte conversion qualifies for NEITHER class:
// the integer-carrier scan (CTS-P3) sees the conversion, the byte-cursor
// scan sees the truth test, and both decline.
// TRUTHMIX: truthmix.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: void pointer parameter

//--- truthmix.c
typedef unsigned char uint8_t;
int truthmix(void *p) {
  if (!p)
    return -1;
  uint8_t *t = p;
  return t[0];
}

// A declaration WITHOUT a body cannot qualify: the admission is a
// body-usage fact of the definition (keeps the Driver ledger golden for
// declaration-only void* signatures stable).
// DECLONLY: declonly.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: void pointer parameter

//--- declonly.c
void fill(void *p, unsigned n);
int main(void) {
  unsigned char b[4];
  fill(b, 4u);
  return 0;
}

// Storing the void* into a global rejects in region analysis BEFORE the
// signature mapping ever runs; the wording names the escape, not the
// parameter type.
// STOREGLOBAL: storeglobal.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global pointer bound to local object 'p' (the borrow would outlive the object)

//--- storeglobal.c
void *g;
int store_it(void *p) {
  g = p;
  return 0;
}

// A void* in a composite position (struct field) stays out: reading the
// field is a pointer-parameter use outside the modeled shapes.
// FIELD: field.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer parameter used outside a direct dereference

//--- field.c
struct holder {
  void *p;
};
int use(struct holder *h) { return h->p != 0; }

// CALL-SITE frontier: an int array is not an admissible byte view of an
// ADMITTED byte-cursor parameter — the element mismatch is a located
// per-call rejection, never a silent cast.
// CALLINT: callsite-int.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: argument element type does not match the slice parameter

//--- callsite-int.c
typedef unsigned char uint8_t;
void bset(void *to, uint8_t val, unsigned len) {
  uint8_t *t = to;
  while (len--)
    *t++ = val;
}
int main(void) {
  int ibuf[4] = {0, 0, 0, 0};
  bset(ibuf, 1, 16u);
  return ibuf[0];
}

// CALL-SITE frontier: the address of a struct object is not a byte view
// either.
// CALLSTRUCT: callsite-struct.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the address of a scalar object cannot be passed as a slice parameter

//--- callsite-struct.c
typedef unsigned char uint8_t;
void bset(void *to, uint8_t val, unsigned len) {
  uint8_t *t = to;
  while (len--)
    *t++ = val;
}
struct S {
  int x;
};
int main(void) {
  struct S s;
  s.x = 0;
  bset(&s, 1, 4u);
  return s.x;
}

// The C++ path is untouched: the admission is gated to C, so the same
// memset-alike shape spelled with static_cast keeps the verbatim
// rejection (C++ void* support would ride a different convention).
// CPP: probe.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: void pointer parameter

//--- probe.cpp
typedef unsigned char uint8_t;
static void byte_set(void *to, uint8_t val, unsigned len) {
  uint8_t *t = static_cast<uint8_t *>(to);
  while (len--)
    *t++ = val;
}
int main() {
  uint8_t b[4];
  byte_set(b, 1, 4u);
  return b[0];
}

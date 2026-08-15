// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/size.c 2>&1 | FileCheck %s --check-prefix=SIZE
// RUN: not emitrust-import-c %t/eltchange.c 2>&1 | FileCheck %s --check-prefix=ELT
// RUN: not emitrust-import-c %t/localbind.c 2>&1 | FileCheck %s --check-prefix=LOCAL
// RUN: not emitrust-import-c %t/row-addr.c 2>&1 | FileCheck %s --check-prefix=ROW
// RUN: not emitrust-import-c %t/vla.c 2>&1 | FileCheck %s --check-prefix=VLA
// RUN: not emitrust-import-c %t/fwd-scalar.c 2>&1 | FileCheck %s --check-prefix=FWDSCALAR

// FR-92 frontier: the 2D-array-pointer cast admits exactly the
// layout-identity shape — a mutable u8 run reinterpreted as
// `[[u8; C]; R]` in call-argument position, SAME element, sufficient
// extent — and the forward admits exactly a pointer-to-constant-size-
// array parameter passed on whole. Everything else keeps a LOCATED
// rejection: rejection is a feature, and none of these shapes may
// silently emit code that is wrong or panics where C was defined. A
// STATICALLY undersized source (u8[8] behind a 16-byte state view) gets
// the NEW import-time arm — without it the runtime helper would panic
// on every execution of a shape the importer could have refused. An
// element-type-CHANGING cast (u8 bytes viewed as u32 rows) is a
// transmute, not a reshape, and keeps the generic located cast
// rejection. A `state_t*` LOCAL binding is not the call-argument shape
// (the admission lives in the borrow-argument lowering only, a
// whole-array reference has no (base, cursor) pointer decomposition)
// and keeps its measured rejection, as does taking the address of one
// ROW. Non-constant dimensions stay out at the type. And the FORWARD
// exception is pointee-keyed: a SCALAR pointer parameter passed on as a
// call argument still classifies as a slice, so the address-of-a-scalar
// call site keeps its measured rejection (no generalization to scalar
// forwarding this wave). Every wording below is pinned verbatim as
// measured against the built tool.

// Statically undersized source: u8[8] cannot back a 4x4 view — the
// import-time extent arm, not a runtime panic.
// SIZE: size.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: cast source region is smaller than the destination array

//--- size.c
typedef unsigned char uint8_t;
typedef uint8_t state_t[4][4];
static void Solo(state_t* state) { (*state)[0][0] = 1; }
void g(void) { uint8_t small[8]; Solo((state_t*)small); }

// Element-type-changing cast: u8 source bytes viewed as u32[4] rows is
// a transmute; the u8-leaf admission gate declines and the generic
// located cast rejection stays.
// ELT: eltchange.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer expression: CStyleCastExpr

//--- eltchange.c
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef uint32_t wide_t[4];
static void W(wide_t* w) { (*w)[0] = 1; }
void g(uint8_t* buf) { W((wide_t*)buf); }

// A `state_t*` LOCAL bound to the cast: not the call-argument shape —
// the whole-array reference has no pointer-local decomposition, so the
// binding keeps its measured rejection.
// LOCAL: localbind.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- localbind.c
typedef unsigned char uint8_t;
typedef uint8_t state_t[4][4];
int f(uint8_t* buf) {
  state_t* s = (state_t*)buf;
  (*s)[0][0] = 1;
  return (*s)[1][2];
}

// The address of one row (`&(*state)[1]`) into a pointer local: the
// row projection is a place, not a region, and the binding keeps its
// measured rejection.
// ROW: row-addr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- row-addr.c
typedef unsigned char uint8_t;
typedef uint8_t state_t[4][4];
void R(state_t* state) {
  uint8_t (*row)[4] = &(*state)[1];
  (*row)[0] = 1;
}

// Non-constant dimensions: a pointer to a VLA has no compile-time
// array type to reference.
// VLA: vla.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-constant array size

//--- vla.c
typedef unsigned char uint8_t;
void V(int n, uint8_t (*m)[n]) { m[0][0] = 1; }

// Differential: the forward exception is for pointer-to-array pointees
// ONLY. A scalar `int*` parameter passed on as a call argument still
// classifies as a slice, so its address-of-a-scalar call site keeps
// the measured rejection.
// FWDSCALAR: fwd-scalar.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the address of a scalar object cannot be passed as a slice parameter

//--- fwd-scalar.c
static void g(int *p) { *p += 1; }
static void f(int *p) { g(p); g(p); }
int main(void) { int x = 3; f(&x); return x; }

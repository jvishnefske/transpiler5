// RUN: split-file %s %t
// RUN: emitrust-import-c --externals-trait %t/lib.c %t/other.c | FileCheck %s

// FR-88 x FR-75 interplay: NULLABLE (Option-wrapped) classification is
// BODY-BASED — a DECLARATION-ONLY function under a trait policy has no
// body to scan, so its requirement signature keeps the plain FR-75
// slice contract (`!emitrust.ref<!emitrust.slice<ui8>>` for the
// const-u8 flavor) and does NOT grow `Option<&[u8]>` by call-site
// consensus: consensus wrapping would let a defining TU (which
// classifies from the body) and a declaration-only TU (which could only
// guess from call sites) emit DIVERGENT signatures for one symbol. A
// DEFINED sibling in the same project still classifies Nullable from
// its body — both outcomes are pinned side by side here so the boundary
// is the presence of a body, nothing else.

//--- lib.c
#include <string.h>
typedef unsigned char uint8_t;

// Declaration-only: stays the FR-75 slice requirement.
int csum(const uint8_t *data, unsigned int n);

// Defined, with the guarded-memcpy body: classifies Nullable.
int absorb(const uint8_t *seed, unsigned int n) {
  uint8_t buf[8] = {0};
  if (seed != 0) {
    memcpy(buf, seed, sizeof buf);
  }
  return (int)buf[0] + (int)n;
}

int use_both(void) {
  uint8_t b[8];
  uint8_t c[8];
  b[0] = 1;
  c[0] = 2;
  return csum(b, 8u) + csum(c, 8u) + absorb(b, 8u) + absorb(0, 0u);
}

//--- other.c
int use_both(void);
int entry(void) {
  return use_both();
}

// The body-less decl survives as a slice-typed requirement — no Option.
// CHECK-DAG: func.func private @csum(!emitrust.ref<!emitrust.slice<ui8>>, ui32) -> i32 attributes {emitrust.external_requirement}

// The defined sibling carries the Option signature from its own body.
// CHECK-DAG: func.func @absorb(%{{.*}}: !emitrust.opaque<"Option<&[u8]>">, %{{.*}}: ui32) -> i32

// Call sites: the requirement takes plain region views; the defined
// callee takes Some(region) / inline None.
// CHECK-LABEL: func.func @use_both
// CHECK: emitrust.slice_of %{{.*}} -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: call @csum(
// CHECK: call @csum(
// CHECK: emitrust.call_opaque "Some"(%{{.*}}) : (!emitrust.ref<!emitrust.slice<ui8>>) -> !emitrust.opaque<"Option<&[u8]>">
// CHECK: call @absorb(
// CHECK: emitrust.constant <#emitrust.opaque<"None">> : !emitrust.opaque<"Option<&[u8]>">
// CHECK: call @absorb(

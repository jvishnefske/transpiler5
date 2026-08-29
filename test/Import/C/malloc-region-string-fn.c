// RUN: emitrust-import-c %s | FileCheck %s

// FR-146: a hosted <string.h> region argument may be an ALLOCATION-BACKED
// `char *`. The W4.2e Part A heap model already decomposes a constant-size
// `malloc`/`calloc` bound to a local pointer into a synthesized entry-block
// MUTABLE `!emitrust.lvalue<!emitrust.array<CAP x i8>>` backing plus an i64
// cursor cell — structurally the SAME place a `char a[N]` region borrows,
// and writable — so `emitrust.slice_of` of that backing at the pointer's
// cursor is the region borrow, byte for byte the local-array lowering.
//
// Before this FR the region borrow only ever consulted the literal backing,
// the FR-72/88 slice-parameter places and the named base object; an
// allocation-backed pointer has NO named base, so it fell into the base
// lookup and SEGFAULTED dereferencing the null `VarDecl` while formatting
// its own rejection (`emitCharRegionSlice`, ImportCHosted.cpp). The crash
// was reachable from two callers — `emitStringCopyCall` (strcpy/strncpy/
// strcat) and `emitMemsetCall`/`emitMemcpyCall` (the byte family) — so both
// caller paths are pinned here, on plain C with no flags.
//
// The two CONTROLS at the bottom pin that the fix does not overreach: a
// STACK char array + strcpy, and a malloc'd pointer with a plain subscript
// store and no string builtin, both keep their historical lowering.

#include <stdlib.h>
#include <string.h>

// The str*-family caller (`emitStringCopyCall`): the destination borrows the
// allocation's backing mutably from its cursor, the source is the literal's
// read-only backing.
int heap_strcpy(void) {
  char *p = (char *)malloc(8);
  strcpy(p, "abc");
  return p[0];
}
// CHECK-LABEL: func.func @heap_strcpy
// CHECK: %[[BACK:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// CHECK: %[[DST:.*]] = emitrust.slice_of mut %[[BACK]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<8xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// CHECK: emitrust.call_opaque "__emitrust_strcpy"(%[[DST]], %{{.*}})

// strcat and strncpy reach the same borrow through the same caller.
int heap_strcat(void) {
  char *p = (char *)malloc(16);
  strcpy(p, "ab");
  strcat(p, "cd");
  strncpy(p, "ef", 2);
  return p[3];
}
// CHECK-LABEL: func.func @heap_strcat
// CHECK: emitrust.call_opaque "__emitrust_strcpy"
// CHECK: emitrust.call_opaque "__emitrust_strcat"
// CHECK: emitrust.call_opaque "__emitrust_strncpy"

// The byte-family caller (`emitMemcpyCall`): the same borrow, through the
// void* argument's implicit bitcast.
int heap_memcpy(void) {
  char *p = (char *)malloc(8);
  memcpy(p, "abc", 3);
  return p[2];
}
// CHECK-LABEL: func.func @heap_memcpy
// CHECK: %[[MBACK:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// CHECK: %[[MDST:.*]] = emitrust.slice_of mut %[[MBACK]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<8xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// CHECK: emitrust.call_opaque "__emitrust_memcpy"(%[[MDST]], %{{.*}}, %{{.*}})

// The byte-family caller (`emitMemsetCall`) — the uthash spelling, inside a
// for loop, on a byte allocation.
int heap_memset(void) {
  char *p = (char *)malloc(8);
  for (int i = 0; i < 2; ++i)
    memset(p, 0, 8);
  p[1] = 3;
  return p[1];
}
// CHECK-LABEL: func.func @heap_memset
// CHECK: %[[SBACK:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// CHECK: %[[SDST:.*]] = emitrust.slice_of mut %[[SBACK]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<8xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// CHECK: emitrust.call_opaque "__emitrust_memset"(%[[SDST]], %{{.*}}, %{{.*}})

// A shared-source position (strlen) borrows the same backing immutably.
int heap_strlen(void) {
  char *p = (char *)malloc(8);
  strcpy(p, "abcd");
  return (int)strlen(p);
}
// CHECK-LABEL: func.func @heap_strlen
// CHECK: %[[LBACK:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// CHECK: emitrust.slice_of %[[LBACK]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<8xi8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>
// CHECK: emitrust.call_opaque "__emitrust_strlen"

// CONTROL 1: a STACK char array destination keeps the historical borrow of
// the array's own place (the machinery that always worked).
int stack_strcpy(void) {
  char b[8];
  strcpy(b, "abc");
  return b[0];
}
// CHECK-LABEL: func.func @stack_strcpy
// CHECK: %[[SB:.*]] = emitrust.variable named "b" : !emitrust.lvalue<!emitrust.array<8xi8>>
// CHECK: emitrust.slice_of mut %[[SB]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<8xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// CHECK: emitrust.call_opaque "__emitrust_strcpy"

// CONTROL 2: a malloc'd pointer with a plain subscript store and no string
// builtin keeps the W4.2e subscript lowering — no slice borrow appears.
int heap_subscript(void) {
  char *p = (char *)malloc(8);
  p[0] = 1;
  return p[0];
}
// CHECK-LABEL: func.func @heap_subscript
// CHECK: %[[PB:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>
// CHECK-NOT: emitrust.slice_of
// CHECK: emitrust.subscript %[[PB]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<8xi8>>, i64) -> !emitrust.lvalue<i8>
// CHECK: return

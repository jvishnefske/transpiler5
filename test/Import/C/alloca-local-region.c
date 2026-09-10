// RUN: split-file %s %t
// RUN: emitrust-import-c %t/hosted.c | FileCheck %s --check-prefix=HOSTED
// RUN: emitrust-import-c --extra-arg=-isystem --extra-arg=%t/inc %t/spelled.c \
// RUN:   | FileCheck %s --check-prefix=SPELLED
// RUN: emitrust-import-c %t/userdef.c | FileCheck %s --check-prefix=USERDEF

// FR-230 (D + G): `alloca` joins the fixed-backing allocation model.
//
// The invariant pinned here is that a stack allocation decomposes into the
// SAME shape `malloc` already does -- a synthesized mutable backing array
// plus an i64 cursor cell -- and nothing more. That is not a widening of the
// model but a better fit for it: the synthesized backing's lifetime IS the
// function's, which is exactly `alloca`'s lifetime, whereas for `malloc` the
// model already shortens a heap lifetime to the frame.
//
// BOTH SPELLINGS ARE PINNED, and that is the load-bearing part of this file.
// `<alloca.h>` defines `alloca` as a function-like MACRO for
// `__builtin_alloca`, so the BUILTIN identifier is what actually reaches the
// importer in hosted code -- a change that recognized only the literal name
// would pass a hand-written declaration and silently refuse every real
// program that includes the header. The literal spelling is reachable too
// (a project's own `void *alloca(unsigned long);` in a system include
// directory), so both are checked; MEASURED, not assumed, because the two
// arrive under different identifiers.
//
// G is pinned too: an allocation-backed region that ALSO receives a null
// constant carries the CTS-P8 Option-of-cursor discriminant (an i1 flag
// cell). Before FR-230 the allocation arm materialized a cursor and a
// backing and NO flag, so `data = NULL;` hit "null pointer constant assigned
// to this pointer" -- a refusal whose own comment claimed the analysis marks
// every null-receiving local region nullable and that the rejection
// therefore only covers non-region pointers. The allocation arm was the
// exception that comment did not cover.

//--- inc/myalloca.h
#ifndef MYALLOCA_H
#define MYALLOCA_H
extern void *alloca(unsigned long);
#endif

//--- hosted.c
#include <alloca.h>
#include <stdlib.h>

// The corpus shape (B01_synthetic/018_stack_buffer_overflow_loop1 `good()`):
// declare, null, allocate, fill, read.
int stack_buffer(void) {
  int *data;
  data = NULL;
  data = (int *)alloca(10 * sizeof(int));
  for (int i = 0; i < 10; i++)
    data[i] = i * i;
  return data[3];
}
// HOSTED-LABEL: func.func @stack_buffer
// The flag cell, the cursor cell, and the mutable backing are all
// synthesized; 40 bytes over a 4-byte element is a [10 x i32].
// HOSTED-DAG: %[[FLAG:.*]] = memref.alloca() : memref<i1>
// HOSTED-DAG: %[[CUR:.*]] = memref.alloca() : memref<i64>
// HOSTED-DAG: %[[BACK:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<10xi32>>
// The declaration binds cursor 0 and flag FALSE -- nothing is allocated yet.
// HOSTED: memref.store %{{.*}}, %[[CUR]][] : memref<i64>
// HOSTED: %[[F0:.*]] = arith.constant false
// HOSTED: memref.store %[[F0]], %[[FLAG]][] : memref<i1>
// `data = NULL` is the None side: only the discriminant is written.
// HOSTED: %[[F1:.*]] = arith.constant false
// HOSTED: memref.store %[[F1]], %[[FLAG]][] : memref<i1>
// `data = alloca(...)` re-zeroes the backing, resets the cursor, and selects
// the Some side. Losing that last store is the miscompile this pins: the
// program would then read as null through an allocation it really holds.
// HOSTED: emitrust.assign %[[BACK]] = %{{.*}}
// HOSTED: memref.store %{{.*}}, %[[CUR]][] : memref<i64>
// HOSTED: %[[TRUE:.*]] = arith.constant true
// HOSTED: memref.store %[[TRUE]], %[[FLAG]][] : memref<i1>
// Every dereference of a nullable region asserts the discriminant first,
// then subscripts the backing directly.
// HOSTED: emitrust.call_opaque "assert!"(%{{.*}}) {args = [0 : index, "null pointer dereference"]}
// HOSTED: emitrust.subscript %[[BACK]]
// HOSTED: return

// A NON-nullable allocation-backed region materializes NO flag cell and no
// assert: the discriminant is state only a null-receiving region pays for.
// This is what keeps every pre-FR-230 malloc lowering byte-identical.
int no_null_no_flag(void) {
  int *p = (int *)alloca(4 * sizeof(int));
  p[0] = 1;
  return p[0];
}
// HOSTED-LABEL: func.func @no_null_no_flag
// HOSTED-NOT: memref<i1>
// HOSTED-NOT: null pointer dereference
// HOSTED: return

//--- spelled.c
// The literal `alloca` identifier, from a declaration the preprocessor never
// turns into `__builtin_alloca`, decomposes identically -- and emits no call
// to any `alloca` symbol.
#include <myalloca.h>

int spelled_alloca(void) {
  int *p = (int *)alloca(4 * sizeof(int));
  p[2] = 9;
  return p[2];
}
// SPELLED-LABEL: func.func @spelled_alloca
// SPELLED: emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
// SPELLED-NOT: call @alloca
// SPELLED: return

//--- userdef.c
// A project that DEFINES its own `alloca` keeps the ordinary-call lowering:
// `asAllocCall`'s `hasBody` guard means a definition of the name is never a
// promotable allocation, exactly as it already was for `malloc`.
long alloca(unsigned long n) { return (long)n; }

long user_defined_alloca(void) { return alloca(16); }
// USERDEF-LABEL: func.func @user_defined_alloca
// USERDEF: call @alloca
// USERDEF: return

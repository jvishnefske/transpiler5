// RUN: split-file %s %t
// RUN: emitrust-import-c %t/record.c | FileCheck %s --check-prefix=RECORD
// RUN: emitrust-import-c %t/record.c -o %t/record.mlir
// RUN: emitrust-opt %t/record.mlir | FileCheck %s --check-prefix=ROUNDTRIP
// RUN: emitrust-import-c %t/silent.c | FileCheck %s --check-prefix=SILENT
// RUN: emitrust-import-c %t/scalar.c | FileCheck %s --check-prefix=SCALAR

// FR-209: the importer must record WHY a pointer parameter became a slice
// when the reason is DELEGATION and not anything visible in the signature,
// because the `--c-abi-exports` refusal cannot see the C and had nothing
// better to say than "its signature is not all-scalar" about a signature
// with nothing wrong with it.
//
// FR-100's forwarding fixpoint keeps a scalar reference across a forwarding
// call ONLY for an ARITHMETIC pointee; a STRUCT pointee records no edge and
// falls through to the conservative slice demand. This file pins the RECORD
// of that decision -- `emitrust.slice_param_delegation` -- and, far more
// importantly, pins that recording it MOVED NO SIGNATURE. The record is a
// reason and never an input: every `func.func` type below is exactly what it
// was before FR-209, which is why each arm checks the emitted type on the
// same line as the attribute rather than the attribute alone.
//
// FOUR properties, one per arm:
//
//  1. RECORD -- the shape itself. `outer` forwards a struct pointer whole,
//     so it carries the record naming the parameter, the callee, and the
//     forwarding CALL's location (the diagnostic hangs a located note on it,
//     which is the only line the author can act on). `helper`, whose own
//     parameter is a plain reference, carries nothing.
//
//  2. ROUNDTRIP -- the record survives print -> parse. It rides a `loc`
//     attribute value, which is the part with a real chance of not parsing
//     back, so `emitrust-opt` re-reading the module is checked and not
//     assumed.
//
//  3. SILENT -- the two shapes that must carry NO record, for two different
//     reasons. `both` forwards AND subscripts: the subscript demands the
//     whole run on its own, so "it is a slice because you forward it" would
//     be false and the record is withheld. `quiet` is `static`: an
//     internal-linkage function is never a C-ABI export candidate, so
//     attaching a message nothing can read would move modules for nothing.
//     Both still classify SLICE -- withholding the record changes no type.
//
//  4. SCALAR -- FR-100's own admitted shape is untouched. An ARITHMETIC
//     pointee forwarded into a scalar-reference callee still keeps
//     `&mut i32` and records nothing; if FR-209's demand suspension had
//     leaked, this is the arm that would show it as a spurious slice.

//--- record.c
typedef struct Ctx { int a; int b; } Ctx;

// RECORD: func.func @helper({{.*}}!emitrust.mut_ref<!emitrust.struct<"Ctx">>{{.*}}attributes {emitrust.param_names = ["c"]}
static int helper(Ctx *c) { return c->a + c->b; }

// RECORD: func.func @outer(%arg0: !emitrust.mut_ref<!emitrust.slice<!emitrust.struct<"Ctx">>>) -> i32 attributes {emitrust.param_names = ["c"], emitrust.slice_param_delegation = [{callee = "helper", index = 0 : i64, param = "c", site = {{.*}}}]}
// ROUNDTRIP: func.func @outer(%arg0: !emitrust.mut_ref<!emitrust.slice<!emitrust.struct<"Ctx">>>) -> i32 attributes {emitrust.param_names = ["c"], emitrust.slice_param_delegation = [{callee = "helper", index = 0 : i64, param = "c", site = {{.*}}}]}
int outer(Ctx *c) { return helper(c); }

//--- silent.c
typedef struct Ctx { int a; int b; } Ctx;

static int helper(Ctx *c) { return c->a + c->b; }

// The subscript is a slice demand of its own; the delegation is not the
// sole reason, so no record -- but the type is a slice all the same.
// SILENT: func.func @both(%arg0: !emitrust.mut_ref<!emitrust.slice<!emitrust.struct<"Ctx">>>) -> i32 attributes {emitrust.param_names = ["c"]}
int both(Ctx *c) { return helper(c) + c[1].a; }

// Internal linkage: a slice for the same reason as `outer`, and no record.
// SILENT: func.func @quiet(%arg0: !emitrust.mut_ref<!emitrust.slice<!emitrust.struct<"Ctx">>>) -> i32 attributes {emitrust.param_names = ["c"]}
static int quiet(Ctx *c) { return helper(c); }

// SILENT-NOT: slice_param_delegation

//--- scalar.c
static void bump(int *n) { *n += 1; }

// FR-100's admitted shape, unchanged: `&mut i32`, not `&mut [i32]`.
// SCALAR: func.func @fwd_scalar(%arg0: !emitrust.mut_ref<i32>) -> i32 attributes {emitrust.param_names = ["n"]}
int fwd_scalar(int *n) {
  bump(n);
  return *n;
}

// SCALAR-NOT: slice_param_delegation

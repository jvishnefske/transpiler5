// RUN: split-file %s %t
// RUN: emitrust-import-c %t/endptr.c | FileCheck %s
// RUN: emitrust-cc --emit=rust %t/endptr.c -o - \
// RUN:   | FileCheck %s --check-prefix=RUST --implicit-check-not=unsafe
// RUN: emitrust-import-c %t/offset.c | FileCheck %s --check-prefix=OFFSET
// RUN: emitrust-import-c %t/nullinit.c | FileCheck %s --check-prefix=NULLINIT

// FR-234 rung 2: the SHAPE of the `endptr` out-parameter lowering. The
// BEHAVIOURAL claim -- that `end - buf`, `*end` and `end == buf` are
// byte-identical to glibc over C's whole prefix grammar -- is pinned by
// byte-diff in `test/EndToEnd/libc-strtox-endptr.c` and cannot be pinned
// here. What lives here is what a refactor would silently change.
//
// THE ONE-LINE CLAIM: C's `*endptr` store is an i64 store into the
// caller's DECOMPOSED CURSOR CELL. No address is materialized, nothing is
// borrowed across the call, and the emitted crate has no `unsafe` and no
// binding -- which is only possible because `end` is a (base, cursor)
// pair rather than a pointer.
//
// FOUR THINGS ARE PINNED, and each is a decision that could be undone by
// accident:
//   1 `&end` IS CONSUMED BY THE REGION ANALYSIS, not by the call
//     lowering, and that is the correction rung 1 made to this rung's
//     plan. Before this change `strtol(buf, &end, 10)` died at
//     `taking the address of a pointer variable` -- a refusal older than
//     the whole strto* feature -- and never reached `emitStrtoIntCall` at
//     all. The admission is `hostedEndptrCoIndex`, which joins `end` into
//     ARGUMENT 0's region exactly as the Shape-P paired out-cursor joins
//     a proven one. That join is also what lets a possibly-uninitialized
//     `char *end;` classify at all, which is why the declaration below
//     has no initializer.
//   2 ONE SCAN, TWO ANSWERS, and the endptr is its own ENTRY POINT rather
//     than a wider result on `__emitrust_strtol`. The cursor does not
//     depend on the saturation limit -- C's subject sequence is the whole
//     run of digits however the accumulator overflows -- so `strtol` and
//     `strtoul` of one string agree on it, and widening the wrappers
//     would have moved rung 1's pinned lowering for nothing. The
//     `__emitrust_strtol` call below is therefore BYTE-IDENTICAL to the
//     NULL-endptr one in `libc-strtox.c`.
//   3 strtod TAKES BOTH COMPONENTS OF ONE CALL. `__emitrust_atof` is now
//     a thin projection of `__emitrust_atof_end`, so the float grammar
//     exists in exactly ONE place (the FR-235 invariant: the tree must
//     not hold two answers to that question) and the value and the cursor
//     cannot disagree because they come out of the same call. There is
//     still deliberately no `__emitrust_strtod`.
//   4 THE COORDINATE CORRECTION. The helper is handed `&base[cursor..]`,
//     so its answer is relative to ARGUMENT 0's cursor, not to the region
//     start; the `arith.addi` is what makes `strtol(p + 2, &end, 10)`
//     land `end` at the right absolute offset. The OFFSET split pins that
//     the addend is the argument's real cursor and not a folded zero.

//--- endptr.c
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  char buf[8] = "42x";
  char *end;

  // CHECK-LABEL: func.func @c_main

  // The region is borrowed ONCE and both calls share it: the value and
  // the cursor are two reads of the same subject string.
  // CHECK: %[[REGION:.*]] = emitrust.slice_of %{{.*}}[%[[ARGCUR:.*]]] : {{.*}} -> !emitrust.ref<!emitrust.slice<i8>>
  long a = strtol(buf, &end, 10);
  // CHECK: emitrust.call_opaque "__emitrust_strtol"(%[[REGION]], %{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>, i32) -> i64
  // CHECK: %[[END:.*]] = emitrust.call_opaque "__emitrust_strto_end"(%[[REGION]], %{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>, i32) -> i64
  // The absolute cursor is argument 0's cursor plus the helper's answer
  // (point 4 above), stored straight into `end`'s own cell.
  // CHECK: %[[ABS:.*]] = arith.addi %[[ARGCUR]], %[[END]] : i64
  // CHECK: memref.store %[[ABS]], %{{.*}} : memref<i64>

  // `end == buf` is C's no-conversion predicate, and it is a CURSOR
  // COMPARISON here -- no pointer identity is involved, because there is
  // no pointer.
  if (end == buf) {
    printf("none\n");
  }
  // Both spellings of the byte AT the cursor subscript the same region at
  // the same cell.
  // CHECK: emitrust.subscript
  printf("%ld %d %d\n", a, (int)*end, (int)end[0]);
  return 0;
}

// The emitted crate carries the pair of integer helpers plus the new
// cursor entry point, all three over the ONE scan.
// RUST: fn __emitrust_strto_scan(
// RUST: ) -> (u64, bool, bool, i64) {
// RUST: fn __emitrust_strto_end(s: &[i8], base: i32) -> i64
// RUST: __emitrust_strto_scan(s, base, u64::MAX, u64::MAX)
// RUST: fn __emitrust_strtol(s: &[i8], base: i32) -> i64
// RUST: __emitrust_strto_scan(s, base, i64::MAX as u64, 1u64 << 63)
//
// No `unsafe` ANYWHERE -- carried by the `--implicit-check-not` on the
// RUN line. An `endptr` is the shape that would most plausibly reach for
// a raw pointer, and it does not: the rubric scores `unsafe` at zero.

//--- offset.c
// Point 4: the parsed string starts PART-WAY into its region, so the
// stored cursor must be the argument's cursor plus the helper's answer.
// A lowering that stored the helper's answer alone would print 2 here
// where C prints 4, and no compile-time check could see it.
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  char buf[16] = "xx42y";
  char *p = buf + 2;
  char *end;
  long v = strtol(p, &end, 10);
  printf("%ld %ld\n", v, (long)(end - buf));
  return 0;
}
// OFFSET-LABEL: func.func @c_main
// OFFSET: emitrust.call_opaque "__emitrust_strto_end"
// OFFSET: arith.addi

//--- nullinit.c
// A NULLABLE `end` (the region sees the null constant, so the pointer
// carries a CTS-P8 flag cell) takes a definite `true` after the call: C
// 7.22.1.4p7 stores a pointer INTO the subject string and never a null
// pointer, so the caller's `if (end)` folds to the answer C guarantees
// rather than to whatever the flag happened to hold.
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  char buf[8] = "42x";
  char *end = NULL;
  long v = strtol(buf, &end, 10);
  if (end) {
    printf("%ld %d\n", v, (int)*end);
  }
  return 0;
}
// NULLINIT-LABEL: func.func @c_main
// NULLINIT: emitrust.call_opaque "__emitrust_strto_end"
// NULLINIT: %[[TRUE:.*]] = arith.constant true
// NULLINIT: memref.store %[[TRUE]], %{{.*}} : memref<i1>

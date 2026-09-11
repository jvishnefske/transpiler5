// RUN: split-file %s %t
// RUN: emitrust-import-c %t/subject.c | FileCheck %s
// RUN: emitrust-cc --emit=rust %t/subject.c -o - \
// RUN:   | FileCheck %s --check-prefix=RUST --implicit-check-not=unsafe
// RUN: emitrust-import-c %t/rebind.c | FileCheck %s --check-prefix=REBIND
// RUN: emitrust-import-c %t/nullend.c | FileCheck %s --check-prefix=NULLEND

// FR-234 rung 3: the SHAPE of `main`'s argv as the subject string of a
// hosted conversion. The BEHAVIOURAL claim -- that the value, the cursor
// and every `end == argv[i]` answer are byte-identical to the clang native
// over real argument vectors -- is pinned by byte-diff in
// `test/EndToEnd/argv-strtox-endptr.c` and cannot be pinned here. What
// lives here is what a refactor would silently change.
//
// THE ONE-LINE CLAIM: an argv argument is a CHAR REGION like any other,
// and a pointer into it is a (WHICH ARGUMENT, byte offset) PAIR. The
// question rung 3 turned on was whether `emitrust.argv_arg`'s
// `&Vec<i8>`-backed borrow and the char-region channel's slice were the
// same representation or two. They are the SAME: `argv_arg` already hands
// back `!emitrust.ref<!emitrust.slice<i8>>`, which is exactly what
// `__emitrust_strtol` takes, so this is an ADAPTER (deref to a slice
// place, reslice at the region cursor) and not a new representation.
//
// FOUR THINGS ARE PINNED, and each is a decision that could be undone:
//   1 THE ADAPTER, not a bespoke argv call path: `argv_arg` -> `deref` ->
//     `slice_of` -> the SAME `__emitrust_strtol`/`__emitrust_strto_end`
//     pair rung 1 and rung 2 emit for a local `char buf[N]`. If a future
//     change gave argv its own helper the float/integer grammars would
//     have two homes again, which is the failure FR-235 exists to prevent.
//   2 THE ARGUMENT INDEX IS RUNTIME STATE -- a SECOND `memref<i64>` cell
//     next to the cursor cell. argv is a run of DISJOINT objects, so a
//     pointer into it cannot be modelled as a bare cursor: at offset 0 a
//     cursor alone cannot tell `argv[1]` from `argv[2]`, and every corpus
//     program's no-conversion test sits exactly there. The `andi` of the
//     index test and the cursor test below IS that fact.
//   3 THE INDEX EXPRESSION IS EVALUATED ONCE per use and shared between
//     the borrow and the identity, so a non-constant index (`argv[argc-1]`
//     here, deliberately not a literal) can never have the two disagree.
//   4 NO `unsafe` AND NO BINDING. The table arrives as `&[Vec<i8>]` and
//     every use borrows a slice of one element; nothing is raw, nothing
//     escapes, and the rubric scores `unsafe` at zero.

//--- subject.c
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  char *end;
  // The admitted signature still carries the argv table (C99-43 C3's seat);
  // rung 3 only widened WHICH uses keep it.
  // CHECK-LABEL: func.func @c_main
  // CHECK-SAME: (%{{.*}}: i32, %[[TABLE:.*]]: !emitrust.argv_table) -> i32
  // Two i64 cells for `end`: the argv argument selector and the byte
  // cursor (point 2). They are distinct allocas.
  // CHECK: memref.alloca() : memref<i64>
  // CHECK: memref.alloca() : memref<i64>

  // A NON-CONSTANT index (point 3), borrowed once and adapted into the
  // ordinary char-region slice (point 1).
  long v = strtol(argv[argc - 1], &end, 10);
  // CHECK: %[[IDX:.*]] = arith.subi %{{.*}}, %{{.*}} : i32
  // CHECK: %[[ARG:.*]] = emitrust.argv_arg %[[TABLE]][%[[IDX]]] : (!emitrust.argv_table, i32) -> !emitrust.ref<!emitrust.slice<i8>>
  // CHECK: %[[PLACE:.*]] = emitrust.deref %[[ARG]] : (!emitrust.ref<!emitrust.slice<i8>>) -> !emitrust.lvalue<!emitrust.slice<i8>>
  // CHECK: %[[BASE:.*]] = arith.constant 0 : i64
  // CHECK: %[[IDX64:.*]] = arith.extsi %[[IDX]] : i32 to i64
  // CHECK: %[[REGION:.*]] = emitrust.slice_of %[[PLACE]][%[[BASE]]] : (!emitrust.lvalue<!emitrust.slice<i8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>
  // The SAME two helpers a local char array gets -- no argv-specific
  // conversion exists (point 1).
  // CHECK: emitrust.call_opaque "__emitrust_strtol"(%[[REGION]], %{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>, i32) -> i64
  // CHECK: %[[END:.*]] = emitrust.call_opaque "__emitrust_strto_end"(%[[REGION]], %{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>, i32) -> i64
  // The endptr write stores BOTH halves of the answer: which argument was
  // parsed, and how far into it the scan got (point 2).
  // CHECK: %[[ABS:.*]] = arith.addi %[[BASE]], %[[END]] : i64
  // CHECK: memref.store %[[IDX64]], %[[SEL:.*]] : memref<i64>
  // CHECK: memref.store %[[ABS]], %[[CUR:.*]] : memref<i64>

  // C's no-conversion predicate is the PAIR test, never the cursor alone.
  if (end == argv[argc - 1])
    return 1;
  // CHECK: %[[CURV:.*]] = memref.load %[[CUR]] : memref<i64>
  // CHECK: %[[SELV:.*]] = memref.load %[[SEL]] : memref<i64>
  // CHECK: %[[SAMEARG:.*]] = arith.cmpi eq, %[[SELV]], %{{.*}} : i64
  // CHECK: %[[SAMEBYTE:.*]] = arith.cmpi eq, %[[CURV]], %{{.*}} : i64
  // CHECK: arith.andi %[[SAMEARG]], %[[SAMEBYTE]] : i1
  printf("%ld\n", v);
  return 0;
}

// The emitted crate takes the table by shared borrow and reslices one
// element per use; nothing raw, nothing escaping (point 4). No `unsafe`
// ANYWHERE -- carried by the `--implicit-check-not` on the RUN line.
// RUST: fn c_main(argc: i32, argv: &[Vec<i8>]) -> i32 {
// RUST: &argv[{{.*}} as usize][..];
// RUST: __emitrust_strtol(
// RUST: __emitrust_strto_end(

//--- rebind.c
// ONE `end` REBOUND ACROSS TWO ARGUMENTS (the 006_static_alias shape).
// This is why the selector is a cell rather than a static fact: the second
// call overwrites it, and both `end == argv[k]` tests have to follow.
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  char *end;
  int k = argc - 2;
  long a = strtol(argv[k], &end, 10);
  int first = end == argv[k];
  long b = strtol(argv[k + 1], &end, 10);
  int second = end == argv[k + 1];
  printf("%ld %ld %d %d\n", a, b, first, second);
  return 0;
}
// REBIND-LABEL: func.func @c_main
// Two borrows, two scans, and the selector cell is stored twice.
// REBIND: emitrust.argv_arg
// REBIND: emitrust.call_opaque "__emitrust_strto_end"
// REBIND: memref.store %{{.*}}, %[[SEL:.*]] : memref<i64>
// REBIND: emitrust.argv_arg
// REBIND: emitrust.call_opaque "__emitrust_strto_end"
// REBIND: memref.store %{{.*}}, %[[SEL]] : memref<i64>

//--- nullend.c
// A NULL endptr over an argv subject (rung 1's admission, form 6 of the
// argv use survey) needs no cell at all: there is no second answer to
// store, so only the value helper is emitted.
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  printf("%ld\n", strtol(argv[argc - 1], (char **)0, 10));
  return 0;
}
// NULLEND-LABEL: func.func @c_main
// NULLEND: emitrust.argv_arg
// NULLEND: emitrust.call_opaque "__emitrust_strtol"
// NULLEND-NOT: __emitrust_strto_end

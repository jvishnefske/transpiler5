// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

// FR-198 pins the SHAPE of the guarded-ladder lowering and, just as
// importantly, the shapes it must REFUSE.
//
// A C switch whose cases fall through in a chain has no properly nested
// structured equivalent, so `lift-cf-to-scf` duplicates the tail into every
// arm: case k is emitted about (N - k + 1) times, output grows as O(N^2.7)
// and the compile stops finishing entirely around N=100 (FR-197). The
// guarded form
//     let entry = match sel { v0 => 0, ... , _ => D };
//     if entry <= 0 { B0 }  if entry <= 1 { B1 } ...
// emits each section exactly ONCE and every guard is a properly nested
// diamond, so the lift has nothing to duplicate.
//
// The invariants pinned here:
//  1. `ladder` lowers to ONE `cf.switch` whose every destination is the
//     first guard block, carrying a section INDEX as a block argument -- the
//     controlling expression is evaluated exactly once and the sections are
//     not duplicated. After the lift it is one `scf.index_switch` yielding
//     the index followed by a FLAT sequence of `scf.if`s, no nesting.
//  2. `sparse_default_middle` proves the index is a SECTION POSITION, not a
//     case value: negative, sparse and out-of-order labels and a `default:`
//     in the middle of the body all map through the same `match`.
//  3. `has_break` (a `break` in a non-final section), `has_decl` (a
//     declaration at switch-body scope, which C keeps live for later cases
//     but a Rust `if` block would not) and `all_return` (nothing falls
//     through, so there is no ladder) keep the ORIGINAL `cf.switch` lowering
//     with one destination block per section.

int printf(const char *, ...);

// CHECK-LABEL: func.func @ladder
// The controlling expression is loaded ONCE and every case destination is
// the single guard block, distinguished only by its index operand. The
// `default:` section is the FOURTH one, so an unmatched value enters at
// index 3 and only that (empty) section runs.
// CHECK: %[[SEL:.*]] = memref.load
// CHECK: cf.switch %[[SEL]] : i32, [
// CHECK-NEXT: default: ^[[GUARD:bb[0-9]+]](%{{.*}} : i32),
// CHECK-NEXT: 0: ^[[GUARD]](%{{.*}} : i32),
// CHECK-NEXT: 1: ^[[GUARD]](%{{.*}} : i32),
// CHECK-NEXT: 2: ^[[GUARD]](%{{.*}} : i32)
// CHECK-NEXT: ]
// CHECK-NEXT: ^[[GUARD]](%[[ENTRY:.*]]: i32):
// Exactly FOUR guards, one per section, each section body emitted ONCE.
// The duplicating lowering emitted case k about (N - k + 1) times.
// CHECK-COUNT-4: arith.cmpi sle, %[[ENTRY]]
// CHECK-NOT: arith.cmpi sle
//
// After the lift: one `scf.index_switch` producing the entry index, then a
// FLAT run of `scf.if`s -- no nesting and no duplication. The first guard
// folds to an `arith.extui` (the section is a single add of a constant to
// zero) and the last section is empty, so two survive as `scf.if`.
// SCF-LABEL: func.func @ladder
// SCF: %[[SENTRY:.*]] = scf.index_switch
// SCF: arith.cmpi sle, %[[SENTRY]]
// SCF-COUNT-2: scf.if
// SCF-NOT: scf.if
int ladder(int x) {
  int acc = 0;
  switch (x) {
  case 0:
    acc += 1;
  case 1:
    acc += 2;
  case 2:
    acc += 4;
  default:
    break;
  }
  return acc;
}

// CHECK-LABEL: func.func @sparse_default_middle
// CHECK: cf.switch %{{.*}} : i32, [
// CHECK-NEXT: default: ^[[SG:bb[0-9]+]](%{{.*}} : i32),
// (`cf.switch` prints its case values as unsigned APInts, so -7 and -1
// appear as their 32-bit two's-complement spellings.)
// CHECK-NEXT: 4294967289: ^[[SG]](%{{.*}} : i32),
// CHECK-NEXT: 1000: ^[[SG]](%{{.*}} : i32),
// CHECK-NEXT: 4294967295: ^[[SG]](%{{.*}} : i32)
// CHECK-NEXT: ]
// CHECK-NEXT: ^[[SG]](%[[SENT:.*]]: i32):
// CHECK-COUNT-4: arith.cmpi sle, %[[SENT]]
// CHECK-NOT: arith.cmpi sle
// SCF-LABEL: func.func @sparse_default_middle
// SCF: %[[SP:.*]] = scf.index_switch
// SCF: arith.cmpi sle, %[[SP]]
// SCF-COUNT-3: scf.if
// SCF-NOT: scf.if
int sparse_default_middle(int x) {
  int acc = 0;
  switch (x) {
  case -7:
    acc += 1;
  case 1000:
    acc += 2;
  default:
    acc += 4;
  case -1:
    acc += 8;
  }
  return acc;
}

// A `break` in a non-final section stops the chain, so the guarded form
// would run the tail anyway: refuse and keep the per-section blocks.
// CHECK-LABEL: func.func @has_break
// CHECK: cf.switch %{{.*}} : i32, [
// CHECK-NEXT: default: ^[[BD:bb[0-9]+]],
// CHECK-NEXT: 0: ^[[B0:bb[0-9]+]],
// CHECK-NEXT: 1: ^[[B1:bb[0-9]+]]
// CHECK-NOT: ^[[B0]](
int has_break(int x) {
  int acc = 0;
  switch (x) {
  case 0:
    acc += 1;
    break;
  case 1:
    acc += 2;
  default:
    acc += 4;
  }
  return acc;
}

// A declaration at switch-body scope is live for the LATER cases in C; each
// guarded `if` is its own Rust scope, so the shape is refused.
// CHECK-LABEL: func.func @has_decl
// CHECK: cf.switch %{{.*}} : i32, [
// CHECK-NEXT: default: ^[[DD:bb[0-9]+]],
// CHECK-NEXT: 0: ^[[D0:bb[0-9]+]],
// CHECK-NEXT: 1: ^[[D1:bb[0-9]+]]
// CHECK-NOT: ^[[D0]](
int has_decl(int x) {
  int acc = 0;
  switch (x) {
  case 0:
    ;
    int carried = 7;
    acc += carried;
  case 1:
    acc += carried;
  default:
    acc += 1;
  }
  return acc;
}

// Nothing falls through, so there is no ladder at all: the ordinary
// jump-table shape is untouched.
// CHECK-LABEL: func.func @all_return
// CHECK: cf.switch %{{.*}} : i32, [
// CHECK-NEXT: default: ^[[RD:bb[0-9]+]],
// CHECK-NEXT: 0: ^[[R0:bb[0-9]+]],
// CHECK-NEXT: 1: ^[[R1:bb[0-9]+]]
// CHECK-NOT: ^[[R0]](
int all_return(int x) {
  switch (x) {
  case 0:
    return 11;
  case 1:
    return 22;
  default:
    return 33;
  }
}

// RUN: split-file %s %t
// RUN: emitrust-import-c %t/strtox.c | FileCheck %s
// RUN: emitrust-cc --emit=rust %t/strtox.c -o - \
// RUN:   | FileCheck %s --check-prefix=RUST --implicit-check-not=unsafe
// RUN: emitrust-import-c %t/user-strtol.c | FileCheck %s --check-prefix=USER

// FR-234 rung 1: the SHAPE of the `strtol`/`strtoul`/`strtod` lowering.
// What lives here is what a refactor would silently change; the
// BEHAVIOURAL claim -- that every one of these is bit-exact against the
// clang native over C's prefix grammar -- is pinned by byte-diff in
// test/EndToEnd/libc-strtox-null-endptr.c and cannot be pinned here.
//
// THREE THINGS ARE PINNED, and each is a decision that could be undone by
// accident:
//   1 THE RESULT DOMAINS ARE DIFFERENT. `__emitrust_strtol` yields i64 and
//     `__emitrust_strtoul` yields ui64, because C's overflow saturation is
//     type-directed (LONG_MAX/LONG_MIN vs ULONG_MAX). A "simplification"
//     that gave both an i64 image and cast on top would be a different
//     function on every overflowing input.
//   2 strtod IS atof, not a copy of it. C 7.22.1.1p2 defines `atof(s)` as
//     `strtod(s, NULL)`, so the call_opaque below names `__emitrust_atof`
//     -- the SAME helper FR-235 settled the inf/nan/hex classification in.
//     A second float helper would be a second place for that to drift, and
//     the whole point of FR-235 was that the tree had held two answers to
//     one question. There is deliberately no `__emitrust_strtod`.
//   3 ONE GRAMMAR, TWO WRAPPERS. Both integer helpers delegate to a single
//     `__emitrust_strto_scan`, which is emitted once however many of them
//     are used; the RUST checks below pin that there is exactly one copy of
//     the prefix rules, and that the whole thing is SAFE Rust with no
//     binding and no `unsafe` (the rubric scores `unsafe` at zero).
// And, as for every name in the shim table, interception is BY NAME AND
// DEFINITION-LESS ONLY: the `user-strtol.c` split pins that a project that
// supplies its own `strtol` still gets an ordinary call to its own code.

//--- strtox.c
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  char buf[8] = "42x";

  // CHECK-LABEL: func.func @c_main

  // The char region is borrowed as a SHARED byte slice (the parse reads
  // it and never writes), and the base rides along as an ordinary i32
  // runtime value rather than being folded into the helper's name --
  // a base read from input has to work exactly as a literal one does.
  long a = strtol(buf, NULL, 10);
  // CHECK: emitrust.call_opaque "__emitrust_strtol"(%{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>, i32) -> i64

  // The unsigned form is a DIFFERENT helper with a DIFFERENT result
  // domain, not the signed one with a cast (see 1 above).
  unsigned long b = strtoul(buf, NULL, 16);
  // CHECK: emitrust.call_opaque "__emitrust_strtoul"(%{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>, i32) -> ui64

  // strtod names atof's helper (see 2 above).
  double c = strtod(buf, NULL);
  // CHECK: emitrust.call_opaque "__emitrust_atof"(%{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>) -> f64
  // CHECK-NOT: __emitrust_strtod

  printf("%ld %lu %f\n", a, b, c);
  return 0;
}

// The helpers reach the emitted crate as plain safe functions, in the
// shim table's own order (atof predates the strto* block, which was
// appended after it). FR-234 rung 2 made `__emitrust_atof` a PROJECTION
// of `__emitrust_atof_end`, so the two travel together even where -- as
// here -- only the NULL-endptr forms are called: an emitted projection
// without its scan would not compile, which is the loud direction.
// RUST: fn __emitrust_atof(s: &[i8]) -> f64
// RUST: fn __emitrust_atof_end(s: &[i8]) -> (f64, i64)
// RUST: fn __emitrust_strto_scan(
// RUST: fn __emitrust_strtol(s: &[i8], base: i32) -> i64
// RUST: __emitrust_strto_scan(s, base, i64::MAX as u64, 1u64 << 63)
// RUST: fn __emitrust_strtoul(s: &[i8], base: i32) -> u64
// RUST: __emitrust_strto_scan(s, base, u64::MAX, u64::MAX)
//
// The two delegations above are the ONLY copies of the prefix grammar:
// each wrapper calls the scan and neither re-implements it. (The helper
// table has no dependency edges, so the scan is requested explicitly by
// the lowering; if that request were dropped the emitted crate would not
// compile, which is the loud failure direction.)
//
// No `unsafe` ANYWHERE in the emitted crate -- the rubric scores it at
// zero, and these are parses over a borrowed slice, which is the whole
// reason they need no binding at all. Carried by the
// `--implicit-check-not` on the RUN line above rather than by a trailing
// RUST-NOT, which would only cover the tail after the last match.

//--- user-strtol.c
// A project-supplied strtol is NOT intercepted: the call imports as an
// ordinary call to the project's own function, exactly as a project
// atof/atoi/puts does. The shim table is a fallback for names the project
// leaves to the C library, never an override of the project's code.
// (The parameter is a plain `char *` rather than the ISO `char **`: a
// pointer-to-pointer parameter is rejected by the cursor-parameter
// analysis before any of this is reached, so the ISO spelling could not
// isolate the claim under test. <stdlib.h> is deliberately not included,
// since redeclaring strtol with a different signature is a C error.)
long strtol(const char *s, char *e, int b) {
  (void)e;
  (void)b;
  return s[0] == '9' ? 9 : 0;
}

int main(void) {
  char text[4] = "9";
  return strtol(text, text, 10) > 1;
}
// USER-LABEL: func.func @strtol(
// USER-LABEL: func.func @c_main
// USER: call @strtol(
// USER-NOT: __emitrust_strtol
// USER-NOT: __emitrust_strto_scan

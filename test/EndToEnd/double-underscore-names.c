// REQUIRES: cargo
// FR-140: differential end-to-end test for C identifiers carrying an
// INTERIOR double underscore. This is THE oracle for the FR: before the
// fix `emitrust-cc --emit=crate` exited 0 on this program and the crate
// then FAILED to build against its own manifest, with 12 errors and no
// diagnostic from the transpiler at all --
//   error: structure field `x__c` should have a snake case name
//   error: function `tu0_mix__up` should have a snake case name
//   error: variable `a__b` should have a snake case name
//   error: variable `i__x` should have a snake case name
//   error: method `tu0_fill__pts` should have a snake case name
// -- because rustc's `is_snake_case` forbids a doubled underscore anywhere
// in the core of a name while `x__c` is perfectly legal C (the TRACTOR
// corpus's `float2half_lib`/`half2float_lib` both carry the shape). Every
// identifier kind that can trip is covered: a free FUNCTION whose FR-73
// per-TU static prefix composes onto the C spelling (`tu0_mix__up`), a
// METHOD on an FR-62 owner struct (`tu0_fill__pts`), PARAMETERS (`a__b`,
// `n__um`, `base__v`), LOCALS including a lifted loop induction variable
// (`i__x`, `sum__all`, `p__ts`), and a struct TYPE with its FIELDS
// (`pt__t` -> `PtT`, `x__c`, `y__c`).
//
// It also pins the two lints that do NOT need covering, both visible in
// this program: the global `g__seed` emits as the thread_local static
// `G__SEED`, which `non_upper_case_globals` (it only ever complains about
// lowercase characters) does not touch, and the struct type emits as
// `PtT`, because `toUpperCamelCase` drops underscores, so
// `non_camel_case_types` never sees a doubled run either.
//
// The crate must now BUILD under the deny set AND produce the same stdout
// as clang compiling the original natively. `cargo build` success alone
// proves nothing -- the fix adds an `#[allow]` attribute, so a build that
// succeeds while the emitted code says something else is exactly the
// failure this diff exists to catch. Every stored value derives from argc
// so constant folding cannot pre-compute the arrays and hide a miscompile;
// the second RUN pair re-seeds through extra argv words.
// Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/double_underscore_names > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/double_underscore_names a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

extern int printf(const char *, ...);

int g__seed = 3;

struct pt__t { int x__c; int y__c; };

static int mix__up(int a__b, int c__d) {
  int t__mp = a__b * 7 + c__d;
  return t__mp ^ (a__b << 2);
}

static void fill__pts(struct pt__t *p__ts, int n__um, int base__v) {
  int i__x;
  for (i__x = 0; i__x < n__um; ++i__x) {
    p__ts[i__x].x__c = mix__up(base__v + i__x, g__seed);
    p__ts[i__x].y__c = p__ts[i__x].x__c - i__x * 3;
  }
}

int main(int argc, char **argv) {
  struct pt__t p__ts[6];
  int i__x;
  int sum__all = 0;
  g__seed = argc * 5 + 1;
  fill__pts(p__ts, 6, argc);
  for (i__x = 0; i__x < 6; ++i__x) {
    printf("%d %d\n", p__ts[i__x].x__c, p__ts[i__x].y__c);
    sum__all += p__ts[i__x].x__c + p__ts[i__x].y__c;
  }
  printf("sum=%d mix=%d\n", sum__all, mix__up(argc, g__seed));
  return 0;
}

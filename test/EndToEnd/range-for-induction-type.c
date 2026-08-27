// FR-61f-7: the `emitrust.for` carries the INDUCTION'S OWN type, not the
// historical i32.
//
// This remainder was recorded as "a design decision needing an op-signature
// change". It was not. `AllTypesMatch<["lowerBound","upperBound","step"]>`
// (EmitRustOps.td) says only that the three operands agree WITH EACH OTHER;
// the operand constraint is already `AnyTypeOf<[AnyInteger, Index]>`, which
// admits every width and signedness; and `emitFor` is type-agnostic. The
// i32-ness was purely `emitRangeFor` hardcoding `getI32Type()` and casting
// the bounds to it.
//
// Every corpus induction in this bucket is UNSIGNED, so this is width and
// signedness work, not sign extension -- and the cheap "cast the bounds to
// i32" shortcut is a MISCOMPILE, not a simplification. Three divergences from
// C are fenced, and this test pins each from BOTH sides:
//
//  (1) TRUNCATION -- a `size_t`/`unsigned long` bound above INT_MAX cast down
//      to i32 changes the trip count outright. Fenced at the root: a bound is
//      never narrowed, it must fit.
//  (2) INTEGER PROMOTION -- in C, `i < HI` on a narrow `i` compares at `int`,
//      which is why `for (uint8_t i = 0; i < 300; i++)` NEVER TERMINATES in C
//      while a u8 range would stop at 255. FR-61f-7 fenced this by refusing
//      every type of lower rank than `int` outright; FR-61f-13 REFINED that to
//      the exact condition, because the blanket refusal was measured as the
//      second-largest remaining blocker (34 unique sites), not the "zero
//      corpus loops" this file originally claimed. Promotion is harmless
//      whenever every value the loop can produce is inside T's range, and two
//      obligations give that: HI must be a compile-time CONSTANT (a runtime
//      `uint8_t` bound may be 255, and then C's final `i += 1` wraps to 0 and
//      loops forever where Rust's `0..255` simply ends), and HI + step must
//      fit T so the increment past the last iteration cannot leave the range.
//  (3) DEFINED WRAPAROUND -- signed overflow is UB, which is what let 61f-3
//      refine `i <= INT_MAX` into a terminating `..=`. UNSIGNED wraparound is
//      DEFINED, so an unsigned induction must additionally prove the
//      increment PAST the last iteration cannot wrap.
//
// `cargo build` success cannot see any of these; only the byte-diff can, so
// every case is diffed against the clang native at three argument values.
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/range_for_induction_type > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/range_for_induction_type a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/range_for_induction_type a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

int printf(const char *, ...);
typedef unsigned long size_t;
typedef unsigned char uint8_t;

// The dominant corpus shape: a plain `unsigned` counter. The head is emitted
// at u32, and the constant bound is MATERIALIZED at u32 rather than emitted
// at i32 and cast -- otherwise the head would read `0i32 as u32..n`.
// CHECK-LABEL: fn u_count
// CHECK:         for {{i|_i}} in 0u32..
// CHECK-NOT:     while
unsigned u_count(unsigned n) {
  unsigned s = 0;
  for (unsigned i = 0; i < n; i++)
    s += i;
  return s;
}

// (1) BOUNDS ABOVE INT_MAX, both of them, held at u64. Cast to i32 the start
// value alone would be mangled beyond recognition.
// CHECK-LABEL: fn wide_bounds
// CHECK:         for {{i|_i}} in 3000000000u64..
unsigned long wide_bounds(unsigned long k) {
  unsigned long s = 0;
  for (unsigned long i = 3000000000UL; i < 3000000000UL + k; i++)
    s += i & 0xffffUL;
  return s;
}

// A `size_t` induction with BOTH bounds unknown at compile time: no cast at
// all, the range head uses the parameters directly.
// CHECK-LABEL: fn sz_runtime
// CHECK:         for {{i|_i}} in lo..hi
size_t sz_runtime(size_t lo, size_t hi) {
  size_t s = 0;
  for (size_t i = lo; i < hi; i++)
    s += i >> 20;
  return s;
}

// A signed `long` induction: i64, and the 61f-3 UB argument still covers the
// final increment, so no extra wraparound obligation applies.
// CHECK-LABEL: fn l_count
// CHECK:         for {{i|_i}} in 0i64..
long l_count(long n) {
  long s = 0;
  for (long i = 0; i < n; i++)
    s += i * 3;
  return s;
}

// (2) THE PROMOTION FENCE. `i` promotes to `int`, so `i < 300` is ALWAYS true
// and this loop never terminates in C -- a u8 range would stop at 255. Refused
// on the type's RANK, before the bound is even considered. (The `break` is a
// guard so the test halts; the refusal does not depend on it.)
// CHECK-LABEL: fn u8_promotes
// CHECK-NOT:     for {{.*}} in
int u8_promotes(int cap) {
  int hits = 0;
  for (uint8_t i = 0; i < 300; i++) {
    hits++;
    if (hits >= cap)
      break;
  }
  return hits;
}

// (2) THE SAME FENCE FROM THE OTHER SIDE, AND THE PIN THAT FLIPPED. This bound
// DOES fit in u8, so the loop is perfectly ordinary. FR-61f-7 refused it
// anyway -- the fence was on the TYPE, not on the bound -- and this file
// asserted that was costless. It was not: the induction-type clause was later
// measured at 34 unique blocked sites. FR-61f-13 refines the fence to the
// exact condition and this loop now lifts, at u8, with `100 + 1 <= 255`.
// The pin is INVERTED rather than deleted: what it guards is that the
// refinement did not simply drop the fence, which `u8_promotes` above and
// `u8_bound_at_max` below pin from the other direction.
// CHECK-LABEL: fn u8_fits_now_lifts
// CHECK:         for {{i|_i}} in 0u8..100u8
int u8_fits_now_lifts(int k) {
  int s = 0;
  for (uint8_t i = 0; i < 100; i++)
    s += i + k;
  return s;
}

// (2c) A constant bound AT the type maximum: the final `i++` wraps to 0 in C
// and loops forever, while a `250u8..255u8` range ends. `HI + step <= max(T)`
// is exactly what refuses it. Guarded so the test halts.
// CHECK-LABEL: fn u8_bound_at_max
// CHECK-NOT:     for {{.*}} in
int u8_bound_at_max(int cap) {
  int hits = 0;
  for (uint8_t i = 250; i < 255; i++) {
    hits++;
    if (hits >= cap)
      break;
  }
  return hits;
}

// (2d) A narrow induction with a RUNTIME bound of its own type. The value is
// in range by construction, but the matcher cannot bound it, so the final
// increment cannot be proved not to wrap. Refused.
// CHECK-LABEL: fn u8_runtime_bound
// CHECK-NOT:     for {{.*}} in
int u8_runtime_bound(unsigned char n, int k) {
  int s = 0;
  for (unsigned char i = 0; i < n; i++)
    s += i + k;
  return s;
}

// (2e) A narrow SIGNED induction, constant bound: same rule, no exemption for
// signed. Converting an out-of-range value to a narrow signed type is
// implementation-defined, not the plain UB that let FR-61f-3 refine `..=`.
// CHECK-LABEL: fn s16_fits
// CHECK:         for {{i|_i}} in 0i16..200i16
int s16_fits(int k) {
  int s = 0;
  for (short i = 0; i < 200; i++)
    s += i + k;
  return s;
}

// (3) THE WRAPAROUND FENCE, constant bound. `i += 7` from a bound this close
// to UINT_MAX wraps past the last iteration and C restarts; Rust's stepped
// range would stop. Refused because HI + step does not fit.
// CHECK-LABEL: fn u_step_wraps
// CHECK-NOT:     for {{.*}} in
unsigned u_step_wraps(unsigned seed) {
  unsigned s = 0;
  unsigned i;
  for (i = 4294967290u; i < 4294967295u; i += 7) {
    s += i ^ seed;
    if (s > 100000000u)
      break;
  }
  return s;
}

// (3) The wraparound fence, runtime bound: a non-unit step cannot prove the
// final increment fits, so only the half-open unit-step shape is admitted.
// CHECK-LABEL: fn u_runtime_step2
// CHECK-NOT:     for {{.*}} in
unsigned u_runtime_step2(unsigned n) {
  unsigned s = 0;
  for (unsigned i = 0; i < n; i += 2)
    s += i;
  return s;
}

// (3) And inclusive on an unsigned runtime bound: `i <= UINT_MAX` loops
// forever in C, so `..=` is NOT a legal refinement the way it was for signed.
// CHECK-LABEL: fn u_runtime_inclusive
// CHECK-NOT:     for {{.*}} in
unsigned u_runtime_inclusive(unsigned n) {
  unsigned s = 0;
  for (unsigned i = 0; i <= n; i++)
    s += i;
  return s;
}

int main(int argc, char **argv) {
  unsigned n = (unsigned)argc + 5u;
  printf("%u\n", u_count(n));
  printf("%lu\n", wide_bounds((unsigned long)argc + 3UL));
  printf("%lu\n", (unsigned long)sz_runtime((size_t)3000000000UL,
                                            (size_t)3000000000UL + n));
  printf("%ld\n", l_count((long)n));
  printf("%d\n", u8_promotes(argc + 400));
  printf("%d\n", u8_fits_now_lifts(argc));
  printf("%d\n", u8_bound_at_max(argc + 9));
  printf("%d\n", u8_runtime_bound((unsigned char)(argc + 7), argc));
  printf("%d\n", s16_fits(argc));
  printf("%u\n", u_step_wraps(n));
  printf("%u\n", u_runtime_step2(n * 3u));
  printf("%u\n", u_runtime_inclusive(n));
  return 0;
}

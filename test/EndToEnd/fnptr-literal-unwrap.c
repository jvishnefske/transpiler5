// FR-133 (clippy::unnecessary_literal_unwrap), BYTE-DIFFED. A fn-ptr LOCAL
// initialized from a literal `Some(f)` loses its `Option` wrapper and every
// use site loses its `.expect("null function pointer")`.
//
// This is CODEGEN, so `cargo build` proves nothing: dropping the wrapper on a
// binding a `None` can still reach compiles perfectly and then calls the wrong
// function (or fails to panic where the C program's UB was refined into a
// panic). Every function below is therefore byte-diffed against the
// clang-built native at three argument counts, and every seed comes from
// `argc` so no call folds to a constant.
//
// The three REFUSAL legs are the safety fence and each is exercised twice --
// once for its emitted shape (the CHECK block) and once for its VALUE (the
// diff):
//   * `reassigned` writes the local a second time. If the fold ignored the
//     reassignment the second call would still dispatch to the first function
//     and print a wrong number.
//   * `null_compared` tests the local against null both ways (`cp == 0` and
//     `!cp`). Those tests are the reason the `Option` exists; unwrapping would
//     leave nothing to compare.
//   * `may_be_none` assigns on ONE arm only, so `None` genuinely reaches the
//     call on the other path. The C source guards the call, and the guard is
//     what keeps the program UB-free at every argc below.
// `multi_arm` records the deliberate scope decision: all-literal assignment on
// both arms of an `if` is a REASSIGNMENT by this FR's rule and keeps its
// `Option`.
//
// `via_parameter` and `via_member` guard the LOCALS-ONLY boundary: a fn-ptr
// parameter and a fn-ptr struct field are typed by a signature and a struct
// definition this rendering fold cannot see, so neither may be unwrapped.
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/fnptr_literal_unwrap > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/fnptr_literal_unwrap a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out
// RUN: %t.native a b c d e f g > %t.n7.out && %t.crate/target/release/fnptr_literal_unwrap a b c d e f g > %t.r7.out
// RUN: diff %t.n7.out %t.r7.out
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

int printf(const char *, ...);

int addk(int a) { return a * 3 + 1; }
int subk(int a) { return a * 5 - 2; }
int mulk(int a) { return a * 7; }

typedef int (*unop)(int);

// The struct field's type is the struct DEFINITION's, never a local's: it
// stays `Option<fn(..)>` whatever any local does.
// CHECK:      struct Calc {
// CHECK-NEXT:     op: Option<fn(i32) -> i32>,
struct Calc {
  unop op;
  int bias;
};

int takes(unop f, int x) { return f(x) + 1; }

/* THE ADMITTED SHAPE: a literal initializer, never reassigned, never
   null-tested, called through twice. */
// The wrapper and both `.expect`s are gone; the constant's own binding is
// unwrapped too, so `Some(` appears nowhere in this function.
// CHECK-LABEL: fn admitted
// CHECK-NEXT:    let v1: fn(i32) -> i32 = addk;
// CHECK-NEXT:    let cp: fn(i32) -> i32 = v1;
// CHECK-NEXT:    let v3: i32 = cp(seed);
// CHECK-NEXT:    let v6: i32 = cp(seed + 1i32);
// CHECK-NEXT:    v3 + v6
int admitted(int seed) {
  unop cp = addk;
  return cp(seed) + cp(seed + 1);
}

/* REASSIGNED: a second write could just as well have been NULL. */
// REFUSAL 1: reassigned. Both writes stay wrapped and both calls keep the
// `.expect` -- a second write could have been NULL.
// CHECK-LABEL: fn reassigned
// CHECK-NEXT:    let v0: Option<fn(i32) -> i32> = Some(addk);
// CHECK-NEXT:    let mut cp: Option<fn(i32) -> i32> = v0;
// CHECK-NEXT:    let r: i32 = cp.expect("null function pointer")(seed);
// CHECK-NEXT:    let v2: Option<fn(i32) -> i32> = Some(subk);
// CHECK-NEXT:    cp = v2;
// CHECK-NEXT:    let v4: i32 = cp.expect("null function pointer")(seed);
int reassigned(int seed) {
  unop cp = addk;
  int r = cp(seed);
  cp = subk;
  return r + cp(seed);
}

/* COMPARED AGAINST NULL, both spellings. */
// REFUSAL 2: compared against null. The `Option` is what `is_none`/`is_some`
// test, so it must survive both spellings of the comparison.
// CHECK-LABEL: fn null_compared
// CHECK-NEXT:    let v2: Option<fn(i32) -> i32> = Some(addk);
// CHECK-NEXT:    let cp: Option<fn(i32) -> i32> = v2;
// CHECK-NEXT:    let v3: Option<fn(i32) -> i32> = cp;
// CHECK-NEXT:    let v5: bool = v3.is_none();
// CHECK:         let v8: Option<fn(i32) -> i32> = cp;
// CHECK-NEXT:    let v10: bool = v8.is_some();
// CHECK:         let v14: i32 = cp.expect("null function pointer")(seed);
int null_compared(int seed) {
  unop cp = addk;
  if (cp == 0)
    return -1;
  if (!cp)
    return -2;
  return cp(seed);
}

/* GENUINELY NULLABLE: assigned on one arm only. The guard keeps it UB-free. */
// REFUSAL 3: genuinely nullable. `None` reaches the binding on one path, so
// the wrapper and the panic-refined `.expect` both stay.
// CHECK-LABEL: fn may_be_none
// CHECK-NEXT:    let v2: Option<fn(i32) -> i32> = None;
// CHECK-NEXT:    let mut cp: Option<fn(i32) -> i32> = v2;
// CHECK:         let v4: Option<fn(i32) -> i32> = Some(mulk);
// CHECK:         let v11: i32 = cp.expect("null function pointer")(seed);
int may_be_none(int seed) {
  unop cp = 0;
  if (seed > 4)
    cp = mulk;
  if (cp)
    return cp(seed);
  return seed - 100;
}

/* MULTI-ARM, ALL LITERAL: deliberately out of scope, keeps its Option. */
// SCOPE DECISION: all-literal assignment on both arms is a REASSIGNMENT by
// this FR's rule and stays wrapped. Admitting it would need definite-
// assignment reasoning this rendering fold does not have.
//
// FR-215 MOVED THE SPELLING, NOT THE DECISION. The declaration used to render
// as a bare `let cp: Option<..>;` with the `if` as a statement below it;
// FR-215's sunk cond-expression fold now binds `cp` at the `if` itself (the
// gap was the inlined `emitrust.cmp` for `seed > 4`). Every load-bearing part
// of this leg is unchanged and still checked: `cp`'s TYPE is still
// `Option<fn(i32) -> i32>`, both arms still construct a literal `Some(..)`,
// and the call still goes through `.expect("null function pointer")`. If
// FR-133 ever over-relaxed and dropped the wrapper, the type on the `let` and
// the `.expect` would both disappear and this leg fails exactly as before.
// CHECK-LABEL: fn multi_arm
// CHECK-NEXT:    let cp: Option<fn(i32) -> i32> = if
// CHECK:         let v2: Option<fn(i32) -> i32> = Some(addk);
// CHECK:         let v3: Option<fn(i32) -> i32> = Some(subk);
// CHECK:         cp.expect("null function pointer")(seed)
int multi_arm(int seed) {
  unop cp;
  if (seed > 4)
    cp = addk;
  else
    cp = subk;
  return cp(seed);
}

/* LOCALS ONLY: a fn-ptr PARAMETER. */
// LOCALS ONLY, leg 1: the PARAMETER's type is the function's signature.
// CHECK-LABEL: fn via_parameter(v0: Option<fn(i32) -> i32>, seed: i32) -> i32 {
// CHECK-NEXT:    let f: Option<fn(i32) -> i32> = v0;
// CHECK-NEXT:    let v3: i32 = f.expect("null function pointer")(seed);
int via_parameter(unop f, int seed) { return f(seed) * 2; }

/* LOCALS ONLY: a fn-ptr STRUCT MEMBER. */
// LOCALS ONLY, leg 2: the STRUCT FIELD keeps the type its definition gives it.
// CHECK-LABEL: fn via_member
// CHECK-NEXT:    let mut c: Calc = Calc::default();
// CHECK-NEXT:    let v0: Option<fn(i32) -> i32> = Some(addk);
// CHECK-NEXT:    c.op = v0;
// CHECK:         c.op.expect("null function pointer")(c.bias)
int via_member(int seed) {
  struct Calc c;
  c.op = addk;
  c.bias = seed;
  return c.op(c.bias);
}

/* An admitted-looking local whose value ESCAPES into a helper. */
// The local's value ESCAPES into a helper whose parameter is
// `Option<fn(i32) -> i32>`; an unwrapped value would not fit it.
// CHECK-LABEL: fn escapes
// CHECK-NEXT:    let v0: Option<fn(i32) -> i32> = Some(subk);
// CHECK-NEXT:    let cp: Option<fn(i32) -> i32> = v0;
// CHECK-NEXT:    takes(cp, seed)
int escapes(int seed) {
  unop cp = subk;
  return takes(cp, seed);
}

int main(int argc, char **argv) {
  int seed = argc;
  printf("admitted %d\n", admitted(seed));
  printf("reassigned %d\n", reassigned(seed));
  printf("null_compared %d\n", null_compared(seed));
  printf("may_be_none %d\n", may_be_none(seed));
  printf("multi_arm %d\n", multi_arm(seed));
  printf("via_parameter %d\n", via_parameter(mulk, seed));
  printf("via_member %d\n", via_member(seed));
  printf("escapes %d\n", escapes(seed));
  return 0;
}

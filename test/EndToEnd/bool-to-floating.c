// REQUIRES: cargo
// Differential end-to-end test pinning that a `_Bool` converted to a
// FLOATING type reaches Rust through the promoted integer, never as a
// direct `bool as f64`.
//
// Two independent defects live on this one edge, and the emitted crate
// could not even be built before the fix:
//
//   1. Rust has no `bool as f32` / `bool as f64` at all -- rustc rejects
//      the spelling outright (E0606), so `emitrust-cc` exited 0 and
//      `cargo build` then failed with "casting `bool` as `f64` is
//      invalid". Silently unbuildable is the one outcome this repo
//      forbids.
//   2. The IR the importer built for it, `arith.sitofp i1 to f64`, is
//      SIGNED: MLIR's sitofp sign-extends its source, so an i1 holding
//      `true` converts to -1.0, not the 1.0 C mandates (C99 6.3.1.4 /
//      6.3.1.2 -- a `_Bool` holds exactly 0 or 1 and widens as an
//      unsigned value). A lowering that merely spelled the cast
//      differently while keeping the signed op would build and print
//      "-1.000000" everywhere.
//
// The correct lowering was already reachable on a SIBLING path: the usual
// arithmetic conversions put an explicit `_Bool` -> `int` node in front of
// `b * 2.5`, so that expression already emitted `v as i32 as f64` and was
// already right. This test drives BOTH paths from the same program, so a
// regression on either diverges the stdout diff.
//
// Every boolean derives from argc, so no constant fold can pre-compute a
// conversion and hide a miscompile; the second RUN pair re-seeds through
// extra argv words to move argc off 1, flipping `f` from false to true and
// exercising the true/false pair in both orders. The trailing integer line
// pins that ordinary int-to-double conversion (the `arith.sitofp i32` case
// that was always correct) is UNCHANGED by the fix.
//
// `cargo build` succeeding proves nothing here beyond defect 1; the stdout
// diff against the clang-built native is the oracle for defect 2.
// --release is load-bearing: debug Rust panics on integer overflow where C
// wraps.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bool_to_floating > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b c > %t.native4.out
// RUN: %t.crate/target/release/bool_to_floating a b c > %t.rust4.out
// RUN: diff %t.native4.out %t.rust4.out

int printf(const char *, ...);

struct Flags {
  _Bool a;
  _Bool b;
};

static _Bool is_odd(int n) { return (n & 1) != 0; }

static double widen(double x) { return x + 0.5; }

/* The conversion in RETURN position: the function result type drives it. */
static double as_double(_Bool b) { return b; }

int main(int argc, char **argv) {
  _Bool t = (argc > 0);  /* always true */
  _Bool f = (argc > 1);  /* false at argc == 1, true at argc == 4 */

  /* Implicit conversion in an initializer, at both float widths. */
  double dt = t;
  double df = f;
  float ft = t;
  float ff = f;

  /* Explicit casts, at both float widths. */
  double ct = (double)t;
  float cf = (float)f;

  /* The already-correct sibling path: the usual arithmetic conversions
     insert a `_Bool` -> `int` node, so these were never broken. They are
     pinned so the fix cannot regress them. */
  double mul = t * 2.5;
  double sum = 1.0 + t;

  /* A struct member and an array element as the conversion source. */
  struct Flags fl;
  fl.a = t;
  fl.b = f;
  double ma = fl.a;
  double mb = fl.b;

  _Bool arr[2];
  arr[0] = t;
  arr[1] = f;
  double a0 = arr[0];
  double a1 = arr[1];

  /* Negation of the converted value: if the conversion sign-extended,
     this would print +1.000000 instead of -1.000000. */
  double neg = -(double)t;

  /* Argument position and return position. */
  double call = widen(t);
  double conv = as_double(is_odd(argc));

  printf("plain %f %f\n", dt, df);
  printf("float %f %f\n", (double)ft, (double)ff);
  printf("cast  %f %f\n", ct, (double)cf);
  printf("arith %f %f\n", mul, sum);
  printf("memb  %f %f\n", ma, mb);
  printf("arr   %f %f\n", a0, a1);
  printf("neg   %f\n", neg);
  printf("call  %f %f\n", call, conv);
  /* Ordinary signed int to double: unchanged by the fix. */
  printf("ints  %f %f\n", (double)argc, (double)(argc - 1));
  return 0;
}

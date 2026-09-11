// RUN: split-file %s %t
// RUN: emitrust-import-c %t/shims.c | FileCheck %s
// RUN: emitrust-cc --emit=rust %t/shims.c -o - | FileCheck %s --check-prefix=RUST
// RUN: emitrust-import-c %t/user-atof.c | FileCheck %s --check-prefix=USERATOF
// RUN: emitrust-cc --emit=rust %t/fputs-stdout.c -o - | FileCheck %s --check-prefix=FPUTS
// RUN: emitrust-cc --emit=rust %t/setlocale-c.c -o - | FileCheck %s --check-prefix=LOCALE

// FR-224: the libc SHIM TABLE. What this file pins is that each of these
// system-header functions lowers to SAFE RUST WITH NO BINDING and no
// `unsafe` -- the whole point of the entry is that eleven of the corpus's
// thirteen system-header blockers need no FFI at all -- and that each one
// is intercepted BY NAME AND DEFINITION-LESS ONLY, so a project that
// supplies its own `atof` still gets an ordinary call to its own code
// (the `user-atof.c` split). The behavioural claims are pinned by
// byte-diff in test/EndToEnd/libc-shim-table.c; what lives HERE is the
// SHAPE of the lowering, which is what a refactor would silently change.

//--- shims.c
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int main(void) {
  char text[16] = "hello world";

  // CHECK-LABEL: func.func @c_main

  // The f32 math forms take the SAME dispatch the f64 ones take, one
  // width down -- an `f32::` opaque callee over an f32 operand, no
  // widening to f64 and back (which would double-round).
  float f = sqrtf(2.0f) + fabsf(-1.5f) + floorf(1.7f) + ceilf(1.2f);
  // CHECK: emitrust.call_opaque "f32::sqrt"(%{{.*}}) : (f32) -> f32
  // CHECK: emitrust.call_opaque "f32::abs"(%{{.*}}) : (f32) -> f32
  // CHECK: emitrust.call_opaque "f32::floor"(%{{.*}}) : (f32) -> f32
  // CHECK: emitrust.call_opaque "f32::ceil"(%{{.*}}) : (f32) -> f32

  // atof takes a shared byte slice into the prefix-parsing helper and
  // yields an f64, exactly as atoi does at i32.
  double d = atof("3.5xyz");
  // CHECK: emitrust.call_opaque "__emitrust_atof"(%{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>) -> f64

  // strcspn/strspn: two SHARED borrows (so the same object may appear
  // twice) into an i64-counting helper, then a cast to C's size_t.
  size_t a = strcspn(text, " ");
  size_t b = strspn(text, "hel");
  // CHECK: emitrust.call_opaque "__emitrust_strcspn"(%{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>, !emitrust.ref<!emitrust.slice<i8>>) -> i64
  // CHECK: emitrust.call_opaque "__emitrust_strspn"(%{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>, !emitrust.ref<!emitrust.slice<i8>>) -> i64

  // div builds the ISO { quot, rem } struct as a place plus two member
  // assignments -- the dialect has no struct-VALUE op -- from the very
  // same divsi/remsi a written `/` and `%` lower to. Both operands are
  // read ONCE and shared by the two arithmetic ops.
  div_t q = div(7, 2);
  // CHECK: %[[DIVPLACE:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"div_t">>
  // CHECK-NEXT: %[[Q:.*]] = arith.divsi
  // CHECK-NEXT: %[[R:.*]] = arith.remsi
  // CHECK-NEXT: %[[QP:.*]] = emitrust.member %[[DIVPLACE]]["quot"]
  // CHECK-NEXT: emitrust.assign %[[QP]] = %[[Q]]
  // CHECK-NEXT: %[[RP:.*]] = emitrust.member %[[DIVPLACE]]["rem"]
  // CHECK-NEXT: emitrust.assign %[[RP]] = %[[R]]
  // CHECK-NEXT: emitrust.load %[[DIVPLACE]] : (!emitrust.lvalue<!emitrust.struct<"div_t">>) -> !emitrust.struct<"div_t">

  printf("%.9g %zu %zu %d %d %f\n", (double)f, a, b, q.quot, q.rem, d);
  return 0;
}
// The requested helpers are emitted once per module, after all imported
// items, and they are ORDINARY SAFE FUNCTIONS -- no `unsafe`, no
// `extern`, no binding.
// CHECK: emitrust.verbatim "fn __emitrust_atof(s: &[i8]) -> f64
// FR-234 rung 2 split the float grammar's ONE copy out into
// `__emitrust_atof_end`, which also answers the consumed length that
// `strtod(s, &e)` needs; `__emitrust_atof` above is now a thin projection
// of its first component. The pair is emitted TOGETHER and in this order
// (the lowering requests both names; the helper table has no dependency
// edges), which this CHECK-NEXT pins -- an emitted `__emitrust_atof`
// without its scan would not compile, and that is the loud direction.
// CHECK-NEXT: emitrust.verbatim "fn __emitrust_atof_end(s: &[i8]) -> (f64, i64)
// FR-235: the two forms of C's strtod grammar that Rust cannot reproduce
// STOP LOUDLY inside that helper rather than answering 0.0. The wording is
// the one FR-229 already settled for `scanf %f`, in the same voice, and it
// is pinned here in the FAST tier so a refactor of the helper text cannot
// quietly retire the refusal -- the runtime behaviour itself is byte-diffed
// in test/EndToEnd/libc-atof-loud-stop.c. The whole helper is ONE verbatim
// string, so these are CHECK-SAME on that same line.
// CHECK-SAME: atof: hexadecimal floating-point input is not supported
// CHECK-SAME: atof: a NaN payload, nan(...), is not supported
// ... and `inf`/`infinity`/`nan` are PARSED, not refused and not dropped:
// IEEE-754 fixes their bits, so the helper returns them directly. They now
// return the value PAIRED with its consumed length (3 for `inf`/`nan`, 8
// for a full `infinity`), which is the only thing rung 2 changed about
// them.
// CHECK-SAME: let v = if neg { -f64::NAN } else { f64::NAN };
// CHECK-SAME: let v = if neg { f64::NEG_INFINITY } else { f64::INFINITY };
// CHECK: emitrust.verbatim "fn __emitrust_strcspn(s: &[i8], reject: &[i8]) -> i64
// CHECK: emitrust.verbatim "fn __emitrust_strspn(s: &[i8], accept: &[i8]) -> i64

// RUST-LABEL: fn c_main() -> i32 {
// RUST-NOT: unsafe
// RUST: DivT { quot: {{.*}}, rem: {{.*}}, }

//--- user-atof.c
// A project-supplied atof is NOT intercepted: the call imports as an
// ordinary func.call to the user definition, exactly as a user-supplied
// abs/atoi/puts does. The shim table is a fallback for names the project
// leaves to libc, never an override of the project's own code.
double atof(const char *s) { return s[0] == '9' ? 9.0 : 0.0; }
int main(void) {
  return atof("9") > 1.0;
}
// USERATOF-LABEL: func.func @atof(
// USERATOF: func.func @c_main
// USERATOF: call @atof(
// USERATOF-NOT: __emitrust_atof

//--- fputs-stdout.c
#include <stdio.h>
// fputs writes the bytes and NO newline; a char REGION takes the raw-byte
// helper (FR-191: the Latin-1 Display funnel doubles every byte >= 0x80),
// while a string literal keeps the plain `print!`.
int main(void) {
  char buf[8] = "hi";
  fputs("lit", stdout);
  fputs(buf, stdout);
  return 0;
}
// FPUTS-LABEL: fn c_main() -> i32 {
// FPUTS: let [[LIT:v[0-9]+]]: &'static str = "lit";
// FPUTS-NEXT: print!("{}", [[LIT]]);
// FPUTS: __emitrust_cstr_out(
// FPUTS-NOT: println!

//--- setlocale-c.c
#include <locale.h>
#include <stdio.h>
// setlocale to the startup locale "C" is elided entirely: it emits
// NOTHING, because it changes nothing observable.
int main(void) {
  setlocale(LC_ALL, "C");
  printf("after\n");
  return 0;
}
// LOCALE-LABEL: fn c_main() -> i32 {
// LOCALE-NEXT: println!("after");
// LOCALE-NOT: setlocale

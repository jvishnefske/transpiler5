// RUN: emitrust-import-c %s | FileCheck %s --implicit-check-not='"__emitrust_cstr_n"'

// C99-47: integer precision, the '+'/' '/'#' flags, the h/hh/ll length
// modifiers, %s precision/width, and %c width.

int printf(const char *fmt, ...);

int main(void) {
  int d = -42;
  unsigned int u = 255u;
  long long ll = 5;
  char buf[8] = "abcdef";

  // Integer precision and sign flags route through the on-demand
  // __emitrust_fmt_i64 helper (value sign-extended to i64; precision,
  // width, and the flag bitmask travel as i32 constants).
  printf("%.5d %+d % d %+10.5d\n", d, d, d, d);
  // CHECK-COUNT-4: emitrust.call_opaque "__emitrust_fmt_i64"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (i64, i32, i32, i32) -> !emitrust.opaque<"String">
  // CHECK: emitrust.call_opaque "println!"({{.*}}) {args = ["{} {} {} {}", 0 : index, 1 : index, 2 : index, 3 : index]}

  // Unsigned precision and '#' route through __emitrust_fmt_u64 (value
  // zero-extended to ui64, with the base as the second argument).
  printf("%#x %#o %.5u %#8.5X\n", u, u, u, u);
  // CHECK-COUNT-4: emitrust.call_opaque "__emitrust_fmt_u64"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (ui64, i32, i32, i32, i32) -> !emitrust.opaque<"String">

  // h/hh reduce the int-promoted argument to short/char range with the
  // same `as`-casts as the width-mismatch rule, then use the 1:1 Rust
  // format-spec fast path.
  printf("%hd %hhd\n", d, d);
  // CHECK: arith.trunci %{{.*}} : i32 to i16
  // CHECK: arith.trunci %{{.*}} : i32 to i8
  // CHECK: emitrust.call_opaque "println!"(%{{.*}}, %{{.*}}) {args = ["{} {}", 0 : index, 1 : index]} : (i16, i8) -> ()
  printf("%hu %04hhx\n", d, d);
  // CHECK: emitrust.cast %{{.*}} : i32 to ui16
  // CHECK: emitrust.cast %{{.*}} : i32 to ui8
  // CHECK: emitrust.call_opaque "println!"(%{{.*}}, %{{.*}}) {args = ["{} {:04x}", 0 : index, 1 : index]} : (ui16, ui8) -> ()

  // ll is 64-bit, identical to l on this target.
  printf("%lld %llx\n", ll, ll);
  // CHECK: emitrust.call_opaque "println!"(%{{.*}}, %{{.*}}) {args = ["{} {:x}", 0 : index, 1 : index]} : (i64, ui64) -> ()

  // %.Ns of a string literal truncates at import time.
  printf("%.3s\n", "abcdef");
  // CHECK: emitrust.literal "\22abc\22"
  // CHECK: emitrust.call_opaque "println!"(%{{.*}}) {args = ["{}", 0 : index]}

  // %.Ns of a char array routes through the raw-bytes __emitrust_cstr_n_out
  // (stops at N bytes or the first NUL, whichever comes first). FR-191: the
  // Display twin __emitrust_cstr_n mapped each byte to a Rust `char`, which
  // re-encoded every byte >= 0x80 as two UTF-8 bytes where C writes one; the
  // width-less %s/%.Ns holes over a char region therefore write raw bytes and
  // the surrounding format text flushes as its own macro call.
  printf("%.4s\n", buf);
  // CHECK: emitrust.call_opaque "__emitrust_cstr_n_out"(%{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>, i64) -> ()
  // CHECK: emitrust.call_opaque "println!"() {args = []}

  // Width on %s and %c right-aligns by default (C's rule; Rust's string
  // formatting would left-align, so the alignment is explicit).
  printf("[%10s][%-10s][%5c][%-5c]\n", "x", "y", 65, 66);
  // CHECK: emitrust.call_opaque "println!"({{.*}}) {args = ["[{:>10}][{:<10}][{:>5}][{:<5}]", 0 : index, 1 : index, 2 : index, 3 : index]}

  return 0;
}

// The helper family is emitted once at module level: the bounded-%s
// helper, then the shared integer core plus the signed and unsigned
// wrappers. Only the raw-bytes bounded helper is requested -- the Display
// twin is unreferenced and must not be emitted (an unused helper is an
// `unused` deny in the emitted crate).
// CHECK: emitrust.verbatim "fn __emitrust_cstr_n_out(s: &[i8], n: i64) {
// CHECK: emitrust.verbatim "fn __emitrust_fmt_int(neg: bool, mag: u64, base: i32, prec: i32,
// CHECK: emitrust.verbatim "fn __emitrust_fmt_i64(x: i64, prec: i32, width: i32, flags: i32) -> String
// CHECK-SAME: x.unsigned_abs()
// CHECK: emitrust.verbatim "fn __emitrust_fmt_u64(x: u64, base: i32, prec: i32, width: i32,

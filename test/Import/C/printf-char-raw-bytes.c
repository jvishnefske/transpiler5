// RUN: split-file %s %t
// RUN: emitrust-import-c %t/stdout.c | FileCheck %s --check-prefix=OUT --implicit-check-not='"__emitrust_fmt_c"'
// RUN: emitrust-import-c %t/buffer.c | FileCheck %s --check-prefix=BUF --implicit-check-not='"__emitrust_byte_out"'

// FR-194: a `%c` directive and a `putchar` write ONE BYTE, and this file
// pins WHERE that byte can come from. C11 7.21.6.1p8 (`%c`) and 7.21.7.9
// (`putchar`) both say the argument is converted to `unsigned char` and that
// one character is written -- exactly one byte for every value 0..255. The
// emitted crate used to route both through `__emitrust_fmt_c`, whose
// `(x as u8) as char` is a Latin-1 widening to a Unicode scalar; `Display
// for char` writes UTF-8, so every byte >= 0x80 became TWO. That is a
// measured miscompile (a 256-value sweep produced 384 emitted bytes against
// 256 native, first differing at offset 0x80), not a theoretical one, and it
// survived because a FileCheck of the IR cannot see it and every `%c` payload
// in the corpus was ASCII -- which is why the load-bearing test for this
// class is the byte differential in test/EndToEnd/printf-char-bytes.c and
// this file only pins the STRUCTURE that differential depends on.
//
// The two groups are deliberately separate, because they are two different
// problems:
//   * stdout.c -- `printf("%c")`, `putchar` and the devirtualized
//     `fprintf(stdout, ...)` alias write the raw byte through
//     `__emitrust_byte_out`, on the same globally buffered handle `print!`
//     locks. The whole-output --implicit-check-not pins that such a module
//     stops requesting `__emitrust_fmt_c` at all (an unused helper is an
//     `unused` deny in the emitted crate).
//   * buffer.c -- `sprintf`/`snprintf` do NOT write stdout, so the raw
//     helper is structurally unavailable to them. They KEEP the Latin-1
//     encoder, which is exact there because `__emitrust_sprintf` walks the
//     formatted `String` one `char` per byte and so inverts the widening
//     rather than encoding it.
// The third entry point of this defect, `std::string += c`, is NOT here: it
// is neither a stdout nor a buffer rendering but a REPRESENTATION limit, and
// it is pinned in test/Import/Cpp/stl-string-highbyte.cpp.

//--- stdout.c
#include <stdio.h>

// CTS-S devirtualized alias of the hosted variadic `fprintf`: the second
// stdout `print!` position (see fnptr-devirt.c).
int (*fprintfptr)(FILE *, const char *, ...) = &fprintf;

static int counter;
int bump(void) {
  counter = counter + 1;
  return counter;
}

// OUT-LABEL: func.func @stdout_bytes
void stdout_bytes(int x) {
  // A bare `%c`: the whole format is the hole, so there is no segment to
  // flush and no `print!` at all.
  printf("%c", x);
  // OUT: %[[B0:.*]] = arith.trunci %{{.*}} : i32 to i8
  // OUT: emitrust.call_opaque "__emitrust_byte_out"(%[[B0]]) : (i8) -> ()

  // Format text on both sides: the pending segment flushes as its own
  // `print!`, the byte goes out raw, and the tail becomes the `println!`.
  printf("[%c]\n", x);
  // OUT: emitrust.call_opaque "print!"() {args = ["["]}
  // OUT: %[[B1:.*]] = arith.trunci %{{.*}} : i32 to i8
  // OUT: emitrust.call_opaque "__emitrust_byte_out"(%[[B1]]) : (i8) -> ()
  // OUT: emitrust.call_opaque "println!"() {args = ["]"]}

  // A FIELD WIDTH is no obstacle for `%c`, unlike `%s`: C pads the ONE byte
  // to the field with spaces and the width is a compile-time constant, so
  // the padding is literal text in the segments AROUND the raw write --
  // before it by default, after it under '-'. (A `%s` field width pads to a
  // length only the run-time formatter knows, which is why that shape keeps
  // the Display funnel; see printf-precision.c.)
  printf("[%5c][%-5c]\n", x, x);
  // OUT: emitrust.call_opaque "print!"() {args = ["[    "]}
  // OUT: %[[B2:.*]] = arith.trunci %{{.*}} : i32 to i8
  // OUT: emitrust.call_opaque "__emitrust_byte_out"(%[[B2]]) : (i8) -> ()
  // OUT: emitrust.call_opaque "print!"() {args = ["]["]}
  // OUT: %[[B3:.*]] = arith.trunci %{{.*}} : i32 to i8
  // OUT: emitrust.call_opaque "__emitrust_byte_out"(%[[B3]]) : (i8) -> ()
  // OUT: emitrust.call_opaque "println!"() {args = ["    ]"]}

  // Definition-less `putchar`: one argument, no format hole, no `print!`.
  putchar(x);
  // OUT: %[[B4:.*]] = arith.trunci %{{.*}} : i32 to i8
  // OUT: emitrust.call_opaque "__emitrust_byte_out"(%[[B4]]) : (i8) -> ()

  // The devirtualized `fprintf(stdout, ...)` alias reaches the same
  // buffered-stdout `print!` context and therefore the same raw write.
  fprintfptr(stdout, "<%c>", x);
  // OUT: emitrust.call_opaque "print!"() {args = ["<"]}
  // OUT: %[[B5:.*]] = arith.trunci %{{.*}} : i32 to i8
  // OUT: emitrust.call_opaque "__emitrust_byte_out"(%[[B5]]) : (i8) -> ()
  // OUT: emitrust.call_opaque "print!"() {args = [">"]}

  // THE ORDERING FENCE, reused from FR-191 rather than reinvented. Flushing
  // the pending segment and writing this byte would move output BEFORE the
  // evaluation of the arguments still to come, where C evaluates every
  // argument before printf writes anything -- and `bump()` is observable. A
  // later argument with side effects therefore DECLINES the raw write and
  // the call stays on the Display funnel, one `println!` after `bump()`.
  // The residual is inherited, not introduced: this shape is still Latin-1
  // for a byte >= 0x80, exactly as FR-191 left the same shape for `%s`.
  printf("%c%d\n", x, bump());
  // OUT: %[[FC:.*]] = emitrust.call_opaque "__emitrust_fmt_c"(%{{.*}}) : (i32) -> !emitrust.opaque<"char">
  // OUT: %[[N:.*]] = call @bump() : () -> i32
  // OUT: emitrust.call_opaque "println!"(%[[FC]], %[[N]]) {args = ["{}{}", 0 : index, 1 : index]}
}

// The Latin-1 encoder IS still emitted here, because the declined call above
// requested it; what the RUN line's --implicit-check-not pins is that no
// CALL to it survives except that one (the verbatim body spells the name
// without the surrounding quotes the pattern requires).
// OUT: emitrust.verbatim "fn __emitrust_fmt_c(x: i32) -> char
// OUT: emitrust.verbatim "fn __emitrust_byte_out(b: i8) {
// FR-228 routed the raw byte through the crate-wide stdout writer:
// sharing `print!`'s LOCK was never enough once the two had to share
// its BUFFERING MODEL as well.
// OUT-SAME: __emitrust_out_write(&[b as u8])

//--- buffer.c
int sprintf(char *dst, const char *fmt, ...);
int snprintf(char *dst, unsigned long size, const char *fmt, ...);

// BUF-LABEL: func.func @buffer_bytes
void buffer_bytes(int x) {
  char b[16];
  // A buffer destination collapses the directives into one `format!` and
  // hands the resulting `String` to the copy helper, so the raw stdout
  // writer cannot be used here at all. The `%c` therefore keeps the Latin-1
  // encoder -- which is EXACT in this position, because the helper below
  // decodes the string one `char` per byte.
  sprintf(b, "a%cb", x);
  // BUF: %[[C0:.*]] = emitrust.call_opaque "__emitrust_fmt_c"(%{{.*}}) : (i32) -> !emitrust.opaque<"char">
  // BUF: %[[F0:.*]] = emitrust.call_opaque "format!"(%[[C0]]) {args = ["a{}b", 0 : index]}
  // BUF: emitrust.call_opaque "__emitrust_sprintf"

  snprintf(b, 16, "%c", x);
  // BUF: %[[C1:.*]] = emitrust.call_opaque "__emitrust_fmt_c"(%{{.*}}) : (i32) -> !emitrust.opaque<"char">
  // BUF: %[[F1:.*]] = emitrust.call_opaque "format!"(%[[C1]]) {args = ["{}", 0 : index]}
  // BUF: emitrust.call_opaque "__emitrust_snprintf"
}

// The copy helpers walk CHARACTERS, not `as_bytes()`. The formatted string
// is Latin-1 by construction (the format literal is restricted to printable
// ASCII, the numeric conversions render ASCII digits, `%c` arrives as
// `__emitrust_fmt_c` and `%s` as `__emitrust_cstr`, both one `char` per
// byte), so one `char` per destination byte inverts the widening exactly.
// `as_bytes()` handed back the UTF-8 ENCODING instead, which stored two
// bytes for a byte >= 0x80 AND counted it twice in the returned length --
// so `sprintf(b, "%c", 0xe9)` wrote two bytes and returned 2 where C writes
// one and returns 1, corrupting every length-dependent computation
// downstream. A code point above 0xff cannot reach here (no non-ASCII string
// data survives import); the assert makes that a loud abort rather than a
// silent truncation if it ever did.
// BUF: emitrust.verbatim "fn __emitrust_fmt_c(x: i32) -> char
// BUF: emitrust.verbatim "fn __emitrust_sprintf(dest: &mut [i8], s: &str) -> i32 {
// BUF-SAME: for ch in s.chars() {
// BUF-SAME: assert!(code < 256
// BUF-SAME: dest[n] = code as u8 as i8;
// BUF-SAME: n as i32
// BUF: emitrust.verbatim "fn __emitrust_snprintf(dest: &mut [i8], size: i64, s: &str) -> i32 {
// BUF-SAME: for ch in s.chars() {
// BUF-SAME: assert!(code < 256
// BUF-SAME: n as i32

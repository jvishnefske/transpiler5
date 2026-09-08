// RUN: emitrust-import-c %s | FileCheck %s --implicit-check-not='"__emitrust_cstr"'

// FR-193 item 2: a `%s` directive carrying a FIELD WIDTH lowers to
// `__emitrust_cstr_pad_out` -- raw payload bytes plus raw space padding
// computed from the C BYTE length of the converted run -- instead of the
// Latin-1 `__emitrust_cstr` Display funnel, which re-encoded every byte
// >= 0x80 as TWO UTF-8 bytes. FR-191 excluded exactly this path, reasoning
// that "a raw `write_all` cannot pad", and recorded it as a scope note; it
// was a live miscompile. Measured on the pre-fix tool for
// `printf("A[%10s]\n", buf)` with buf = 81 8e 9b a8 7a:
//     native : 41 5b 20 20 20 20 20 81 8e 9b a8 7a 5d 0a
//     emitted: 41 5b 20 20 20 20 20 c2 81 c2 8e c2 9b c2 a8 7a 5d 0a
// The pad COUNT was already right (`__emitrust_cstr` yields one Rust `char`
// per C byte and Rust's `{:>10}` pads by char count); only the payload
// ENCODING diverged. The raw path therefore has to reproduce the same pad
// arithmetic, which is why the width, the precision and the '-' flag all
// travel as i32 constants -- '*' width/precision and the '0' flag are
// located rejections, so every one of them is known at import time and only
// the run length is a runtime quantity.
//
// The byte-level differential that actually proves the repair is
// test/EndToEnd/printf-string-width-nonascii.c; this file pins the LOWERING
// so a regression shows up as an IR shape change, not only as a diff of a
// built binary.
//
// The `--implicit-check-not` is load-bearing: every `%s` in this file must
// take the raw path, so the module must stop requesting `__emitrust_cstr`
// altogether (an unused helper is an `unused` deny in the emitted crate).
// It matches the QUOTED callee name, so it does not collide with the
// `__emitrust_cstr_pad_out`/`_out`/`_n_out` helper names.

#include <stdio.h>

char gwbuf[8];

// CHECK-LABEL: func.func @width_shapes
void width_shapes(void) {
  char buf[8];
  buf[0] = (char)0xff;
  buf[1] = 'z';
  buf[2] = 0;

  // Right alignment (C's default) is flags == 0; the pending segment
  // flushes as its own `print!` first, exactly like the unpadded bypass.
  printf("[%10s]\n", buf);
  // CHECK: %[[SL0:.*]] = emitrust.slice_of %{{.*}} : (!emitrust.lvalue<!emitrust.array<8xi8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>
  // CHECK: emitrust.call_opaque "print!"() {args = ["["]}
  // CHECK: %[[P0:.*]] = arith.constant -1 : i32
  // CHECK: %[[W0:.*]] = arith.constant 10 : i32
  // CHECK: %[[F0:.*]] = arith.constant 0 : i32
  // CHECK: emitrust.call_opaque "__emitrust_cstr_pad_out"(%[[SL0]], %[[P0]], %[[W0]], %[[F0]]) : (!emitrust.ref<!emitrust.slice<i8>>, i32, i32, i32) -> ()
  // CHECK: emitrust.call_opaque "println!"() {args = ["]"]}

  // The '-' flag is bit 1 of the same flag bitmask the `__emitrust_fmt_*`
  // helpers use. No other flag can reach here: '+'/' '/'#' are rejected on
  // `%s` by C99 7.19.6.1p6, and '0' is undefined there and rejected too.
  printf("[%-10s]\n", buf);
  // CHECK: emitrust.call_opaque "print!"() {args = ["["]}
  // CHECK: %[[P1:.*]] = arith.constant -1 : i32
  // CHECK: %[[W1:.*]] = arith.constant 10 : i32
  // CHECK: %[[F1:.*]] = arith.constant 1 : i32
  // CHECK: emitrust.call_opaque "__emitrust_cstr_pad_out"(%{{.*}}, %[[P1]], %[[W1]], %[[F1]]) : (!emitrust.ref<!emitrust.slice<i8>>, i32, i32, i32) -> ()

  // Width AND precision: a non-negative precision bounds the run, and the
  // pad is then computed from the TRUNCATED byte length. `-1` is the
  // "no precision" spelling, so the two parameters cannot be confused.
  printf("[%10.3s]\n", buf);
  // CHECK: %[[P2:.*]] = arith.constant 3 : i32
  // CHECK: %[[W2:.*]] = arith.constant 10 : i32
  // CHECK: %[[F2:.*]] = arith.constant 0 : i32
  // CHECK: emitrust.call_opaque "__emitrust_cstr_pad_out"(%{{.*}}, %[[P2]], %[[W2]], %[[F2]]) : (!emitrust.ref<!emitrust.slice<i8>>, i32, i32, i32) -> ()

  printf("[%-10.3s]\n", buf);
  // CHECK: %[[P3:.*]] = arith.constant 3 : i32
  // CHECK: %[[W3:.*]] = arith.constant 10 : i32
  // CHECK: %[[F3:.*]] = arith.constant 1 : i32
  // CHECK: emitrust.call_opaque "__emitrust_cstr_pad_out"(%{{.*}}, %[[P3]], %[[W3]], %[[F3]]) : (!emitrust.ref<!emitrust.slice<i8>>, i32, i32, i32) -> ()

  // Precision ZERO is a real value, not "absent": the run is empty and the
  // whole field is padding.
  printf("[%6.0s]\n", buf);
  // CHECK: %[[P4:.*]] = arith.constant 0 : i32
  // CHECK: %[[W4:.*]] = arith.constant 6 : i32
  // CHECK: emitrust.call_opaque "__emitrust_cstr_pad_out"(%{{.*}}, %[[P4]], %[[W4]], %{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>, i32, i32, i32) -> ()

  // A mid-buffer `&buf[i]` and a staged global copy reach the padded raw
  // path through the same slice shapes the unpadded bypass admits.
  printf("[%4s]\n", &buf[1]);
  // CHECK: %[[SL5:.*]] = emitrust.slice_of
  // CHECK: emitrust.call_opaque "__emitrust_cstr_pad_out"(%[[SL5]], %{{.*}}, %{{.*}}, %{{.*}})
  printf("[%4s]\n", gwbuf);
  // CHECK: emitrust.global_load @gwbuf
  // CHECK: emitrust.call_opaque "__emitrust_cstr_pad_out"

  // Format text on both sides plus a trailing `%d`: the padded raw write
  // lands BETWEEN the two segments, so the surrounding literal text keeps
  // its order relative to the payload.
  printf("pre[%6s]post %d\n", buf, 3);
  // CHECK: emitrust.call_opaque "print!"() {args = ["pre["]}
  // CHECK: emitrust.call_opaque "__emitrust_cstr_pad_out"
  // CHECK: emitrust.call_opaque "println!"(%{{.*}}) {args = ["]post {}", 0 : index]}

  // An unpadded `%s` in the same module still takes the plain helper: the
  // padded form is an addition, not a replacement.
  printf("[%s]\n", buf);
  // CHECK: emitrust.call_opaque "__emitrust_cstr_out"(%{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>) -> ()

  // The `%.Ns`-only form likewise keeps its own helper and its i64 count.
  printf("[%.2s]\n", buf);
  // CHECK: %[[N:.*]] = arith.constant 2 : i64
  // CHECK: emitrust.call_opaque "__emitrust_cstr_n_out"(%{{.*}}, %[[N]]) : (!emitrust.ref<!emitrust.slice<i8>>, i64) -> ()
}

// CHECK-LABEL: func.func @literal_keeps_formatter
void literal_keeps_formatter(void) {
  // A string LITERAL argument is a `&'static str`, never entered the byte
  // funnel, and is ASCII by import rule -- so a width on it stays a plain
  // Rust format placeholder. Nothing here requests `__emitrust_cstr`.
  printf("[%10s][%-10s][%10.3s]\n", "abc", "abc", "abcdef");
  // The precision on a literal is applied AT IMPORT -- the third operand is
  // the already-truncated `"abc"`, not `"abcdef"` under a `{:.3}` -- so the
  // placeholder carries the width alone.
  // CHECK: emitrust.literal "\22abc\22" : !emitrust.opaque<"&'static str">
  // CHECK: emitrust.literal "\22abc\22" : !emitrust.opaque<"&'static str">
  // CHECK: emitrust.literal "\22abc\22" : !emitrust.opaque<"&'static str">
  // CHECK: emitrust.call_opaque "println!"({{.*}}) {args = ["[{:>10}][{:<10}][{:>10}]", 0 : index, 1 : index, 2 : index]}
}

// CHECK: fn __emitrust_cstr_pad_out(s: &[i8], prec: i32, width: i32,

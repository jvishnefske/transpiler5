// RUN: emitrust-import-c %s | FileCheck %s

// FR-191: a `%s` argument that designates a whole `char` region prints its
// RAW BYTES through `__emitrust_cstr_out` (or `__emitrust_cstr_n_out` under a
// `%.Ns`) instead of the `__emitrust_cstr` Latin-1 `Display` funnel, whose
// per-byte `u8 as char` widening re-encodes every byte >= 0x80 as TWO UTF-8
// bytes -- a measured miscompile, not a theoretical one (native `ff fe 81 7a`
// vs emitted `c3 bf c3 be c2 81 7a`). The bypass already existed for argv
// (C99-43 C3); this pins the widening to every stdout `%s` region shape, the
// `print!` segment flushing around the raw write, and the two shapes that
// deliberately KEEP the Display funnel.
//
// FR-193 item 2 moved one of those pins FORWARD: a FIELD WIDTH used to be a
// third KEEP case here, on the reasoning that "a raw `write_all` cannot pad".
// It is now `__emitrust_cstr_pad_out` at the bottom of @raw_bytes -- see the
// comment there for why the reasoning was true and the conclusion wrong.
//
// The helper request gating is part of the contract: a module whose every
// `%s` takes the bypass must stop emitting `__emitrust_cstr` (an unused
// helper is an `unused` deny in the emitted crate).

#include <stdio.h>

// CTS-S devirtualized alias of the hosted variadic `fprintf`: a call
// through it routes into the printf machinery with the `stdout` argument
// swallowed (see fnptr-devirt.c), which is the second stdout `print!`
// position FR-191 widens.
int (*fprintfptr)(FILE *, const char *, ...) = &fprintf;

static int counter;
int bump(void) {
  counter = counter + 1;
  return counter;
}

// CHECK-LABEL: func.func @raw_bytes
void raw_bytes(void) {
  char buf[8];
  buf[0] = (char)0xff;
  buf[1] = 'z';
  buf[2] = 0;

  // A whole char array: the pending format segment flushes as its own
  // `print!`, the region's bytes go out raw, and translation continues into
  // a fresh segment.
  printf("[%s]\n", buf);
  // CHECK: %[[SL0:.*]] = emitrust.slice_of %{{.*}} : (!emitrust.lvalue<!emitrust.array<8xi8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>
  // CHECK: emitrust.call_opaque "print!"() {args = ["["]}
  // CHECK: emitrust.call_opaque "__emitrust_cstr_out"(%[[SL0]]) : (!emitrust.ref<!emitrust.slice<i8>>) -> ()
  // CHECK: emitrust.call_opaque "println!"() {args = ["]"]}

  // `&buf[i]`: the same raw funnel from element i.
  printf("%s\n", &buf[1]);
  // CHECK: %[[SL1:.*]] = emitrust.slice_of
  // CHECK: emitrust.call_opaque "__emitrust_cstr_out"(%[[SL1]])
  // The whole format tail was consumed by the bypass, so the trailing
  // newline folds into a bare `println!()` -- no dead `print!("")`.
  // CHECK: emitrust.call_opaque "println!"() {args = []}

  // `%.Ns` bounds the raw run at N bytes (or an earlier NUL).
  printf("%.2s|", buf);
  // CHECK: %[[N:.*]] = arith.constant 2 : i64
  // CHECK: emitrust.call_opaque "__emitrust_cstr_n_out"(%{{.*}}, %[[N]]) : (!emitrust.ref<!emitrust.slice<i8>>, i64) -> ()
  // CHECK: emitrust.call_opaque "print!"() {args = ["|"]}

  // The devirtualized `fprintf(stdout, ...)` form writes the SAME stdout
  // `print!` stream, so it takes the bypass too.
  fprintfptr(stdout, "f%s\n", buf);
  // CHECK: emitrust.call_opaque "print!"() {args = ["f"]}
  // CHECK: emitrust.call_opaque "__emitrust_cstr_out"
  // CHECK: emitrust.call_opaque "println!"() {args = []}

  // `puts` is the same funnel in the same stdout position: raw bytes, then
  // the newline the macro writes.
  puts(buf);
  // CHECK: emitrust.call_opaque "__emitrust_cstr_out"
  // CHECK: emitrust.call_opaque "println!"() {args = []}

  // FR-193 item 2 MOVED THIS PIN FORWARD. This file used to assert, under
  // `keeps_display_funnel`, that a FIELD WIDTH kept the Latin-1 funnel
  // because "the raw write cannot pad". It cannot pad on its own -- but C
  // pads to the BYTE length of the converted run, which is exactly the
  // length the helper's NUL scan already computes, so the padding and the
  // payload go out as one raw run through `__emitrust_cstr_pad_out`.
  // Leaving it on the funnel was a silent miscompile, measured: for
  // `%10s` over `81 8e 9b a8 7a` the native writes five pad bytes then
  // five payload bytes, the funnel wrote five pad bytes then NINE.
  printf("[%10s]\n", buf);
  // CHECK: emitrust.call_opaque "print!"() {args = ["["]}
  // CHECK: %[[P0:.*]] = arith.constant -1 : i32
  // CHECK: %[[W0:.*]] = arith.constant 10 : i32
  // CHECK: %[[F0:.*]] = arith.constant 0 : i32
  // CHECK: emitrust.call_opaque "__emitrust_cstr_pad_out"(%{{.*}}, %[[P0]], %[[W0]], %[[F0]]) : (!emitrust.ref<!emitrust.slice<i8>>, i32, i32, i32) -> ()
  // CHECK: emitrust.call_opaque "println!"() {args = ["]"]}
}

// CHECK-LABEL: func.func @keeps_display_funnel
void keeps_display_funnel(void) {
  char buf[8];
  char out[16];
  buf[0] = 'a';
  buf[1] = 0;

  // A LATER argument with side effects: C evaluates every argument before
  // printf writes anything, so flushing a segment mid-directive-scan would
  // reorder that argument's own effects against this call's output. Such a
  // call keeps the single-`print!` shape.
  printf("%s %d\n", buf, bump());
  // CHECK: emitrust.call_opaque "__emitrust_cstr"
  // CHECK: emitrust.call_opaque "println!"({{.*}}) {args = ["{} {}", 0 : index, 1 : index]}

  // `sprintf` collapses its directives into a `format!` String that is
  // copied into a char buffer -- NOT stdout -- so the raw stdout funnel is
  // unavailable there and the Display funnel stays.
  sprintf(out, "<%s>", buf);
  // CHECK: emitrust.call_opaque "__emitrust_cstr"
  // CHECK: emitrust.call_opaque "format!"
}

// CHECK-LABEL: func.func @literal_arg
void literal_arg(void) {
  // A string LITERAL argument is already a `&'static str` in the format
  // hole: it never reached the Latin-1 funnel and is not rerouted.
  printf("%s\n", "abc");
  // CHECK: %[[L:.*]] = emitrust.literal "\22abc\22" : !emitrust.opaque<"&'static str">
  // CHECK: emitrust.call_opaque "println!"(%[[L]]) {args = ["{}", 0 : index]}
  // CHECK-NOT: __emitrust_cstr_out
}

// Both funnels are live in this module, so both helpers are emitted once.
// CHECK: emitrust.verbatim "fn __emitrust_cstr(s: &[i8]) -> String
// CHECK: emitrust.verbatim "fn __emitrust_cstr_out(s: &[i8]) {
// CHECK: emitrust.verbatim "fn __emitrust_cstr_n_out(s: &[i8], n: i64) {

// RUN: emitrust-import-c %s | FileCheck %s

// FR-94: a flexible-array-member record with an allocation-backed tail
// (heatshrink's `struct heatshrink_decoder { fields...; uint8_t buffers[]; }`
// shape). The admitted subset — a gap-free (offsetof(tail) == sizeof) struct
// whose FAM leaf is `unsigned char` — grows a trailing owned `Vec<u8>` field
// in its struct_def, and the five heatshrink access-site shapes lower over it:
//   1. `S *d = malloc(sizeof(S) + n)` (runtime n) becomes an OWNED struct
//      local whose tail member is `vec![0u8; n]` (`emitrust.vec_fill`, the
//      FR-65 zero-refinement of malloc's indeterminate bytes); the
//      malloc-failure null guard is elided (vec! is infallible, the FR-65
//      precedent) and `free(d)` is a no-op drop.
//   2. `return d` lifts the function to an OWNED struct return (the tail
//      rides along; callers bind the result to their own owned local).
//   3. `d->buffers[i]` is member + subscript over the Vec field.
//   4. `memset(d->buffers, 0, n)` / `memcpy(&d->buffers[k], src, n)` are
//      byte-family slice windows over the Vec member (the FR-74/86/87
//      member-region machinery).
//   5. `uint8_t *p = &d->buffers[k]` binds a cursored pointer local over the
//      member place (the FR-93 member-array backing, tail-extended).
// A `free`-only wrapper types its parameter OWNED BY VALUE (the free is the
// drop; a caller's use-after-free becomes rustc E0382, the loud direction).
// This file pins every admitted shape; the frontier arms stay in
// flexible-array-owned-tail-invalid.c and flexible-array-invalid.c.

#include <stdlib.h>
#include <string.h>

typedef struct {
  unsigned short input_size;
  unsigned short input_index;
  unsigned char window_sz2;
  unsigned short input_buffer_size;
  unsigned char buffers[];
} dec;

// The struct_def grows the owned tail field at the FAM's position.
// CHECK: emitrust.struct_def @dec ["input_size", "input_index", "window_sz2", "input_buffer_size", "buffers"] [ui16, ui16, ui8, ui16, !emitrust.opaque<"Vec<u8>">]

dec *dec_alloc(unsigned char w, unsigned short ibs) {
  size_t n = ((size_t)1 << w) + ibs;
  dec *d = malloc(sizeof(dec) + n);
  if (!d) return NULL;
  d->input_size = 0;
  d->input_index = 0;
  d->window_sz2 = w;
  d->input_buffer_size = ibs;
  memset(d->buffers, 0, n);
  return d;
}

// The signature lifts to an owned struct return; the tail member binds the
// runtime-sized zero fill, and the malloc-failure branch (`return NULL`) is
// elided, so no conditional survives.
// CHECK-LABEL: func.func @dec_alloc
// CHECK-SAME: -> !emitrust.struct<"dec">
// CHECK-NOT: cf.cond_br
// CHECK: %[[TAIL:.*]] = emitrust.member %[[D:.*]]["buffers"] : (!emitrust.lvalue<!emitrust.struct<"dec">>) -> !emitrust.lvalue<!emitrust.opaque<"Vec<u8>">>
// CHECK: %[[FILL:.*]] = emitrust.vec_fill "0u8", %{{.*}} : i64 -> <"Vec<u8>">
// CHECK: emitrust.assign %[[TAIL]] = %[[FILL]] : !emitrust.lvalue<!emitrust.opaque<"Vec<u8>">>
// The memset is a mutable byte window over the Vec member.
// CHECK: emitrust.slice_of mut %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<u8>">>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK: call_opaque "__emitrust_memset_u8"
// CHECK: %[[RET:.*]] = emitrust.load %[[D]] : (!emitrust.lvalue<!emitrust.struct<"dec">>) -> !emitrust.struct<"dec">
// CHECK: return %[[RET]] : !emitrust.struct<"dec">

void dec_free(dec *d) { free(d); }

// The free-only wrapper takes the record OWNED BY VALUE and emits no free
// call — dropping the parameter IS the deallocation.
// CHECK-LABEL: func.func @dec_free
// CHECK-SAME: (%{{.*}}: !emitrust.struct<"dec">)
// CHECK-NOT: call
// CHECK: return

void dec_sink(dec *d, const unsigned char *in, unsigned len) {
  memcpy(&d->buffers[d->input_size], in, len);
  d->input_size += len;
}

// The sink site: a mutable Vec-member window at the runtime member offset,
// copied into via the u8 memcpy helper.
// CHECK-LABEL: func.func @dec_sink
// CHECK: emitrust.slice_of mut %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<u8>">>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK: call_opaque "__emitrust_memcpy_u8"

unsigned char dec_next(dec *d) {
  return d->buffers[d->input_index++];
}

// The get_bits site: member + subscript over the Vec field through the
// ordinary `&mut S` parameter convention.
// CHECK-LABEL: func.func @dec_next
// CHECK: %[[VT:.*]] = emitrust.member %{{.*}}["buffers"] : (!emitrust.lvalue<!emitrust.struct<"dec">>) -> !emitrust.lvalue<!emitrust.opaque<"Vec<u8>">>
// CHECK: %[[CELL:.*]] = emitrust.subscript %[[VT]][%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<u8>">>, ui16) -> !emitrust.lvalue<ui8>
// CHECK: emitrust.load %[[CELL]] : (!emitrust.lvalue<ui8>) -> ui8

unsigned dec_window(dec *d) {
  unsigned char *buf = &d->buffers[d->input_buffer_size];
  unsigned s = 0;
  for (unsigned i = 0; i < 4u; i++) {
    buf[i] = (unsigned char)(buf[i] + 1u);
    s += buf[i];
  }
  return s;
}

// CHECK-LABEL: func.func @dec_window
// The poll-site window local: a cursored pointer over the member place; each
// access subscripts the Vec member at (cursor + i).
// CHECK: emitrust.member %{{.*}}["buffers"] : (!emitrust.lvalue<!emitrust.struct<"dec">>) -> !emitrust.lvalue<!emitrust.opaque<"Vec<u8>">>
// CHECK: emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<u8>">>, i64) -> !emitrust.lvalue<ui8>

unsigned caller(void) {
  dec *d = dec_alloc(4, 8);
  if (!d) return 1;
  unsigned char msg[2] = {7, 9};
  dec_sink(d, msg, 2u);
  unsigned v = dec_next(d);
  dec_free(d);
  return v;
}

// The caller binds the owned return to its own struct local, passes it by
// `&mut` to the sink/next conventions, and MOVES it into the owned free
// wrapper; the null guard on the (infallible) owned return is elided.
// CHECK-LABEL: func.func @caller
// CHECK-NOT: cf.cond_br
// CHECK: %[[CD:.*]] = call @dec_alloc(%{{.*}}, %{{.*}}) : (ui8, ui16) -> !emitrust.struct<"dec">
// CHECK: emitrust.assign %[[DP:.*]] = %[[CD]] : !emitrust.lvalue<!emitrust.struct<"dec">>
// CHECK: emitrust.addr_of mut %[[DP]] : (!emitrust.lvalue<!emitrust.struct<"dec">>) -> !emitrust.mut_ref<!emitrust.struct<"dec">>
// CHECK: call @dec_sink
// CHECK: call @dec_next
// CHECK: %[[MV:.*]] = emitrust.load %[[DP]] : (!emitrust.lvalue<!emitrust.struct<"dec">>) -> !emitrust.struct<"dec">
// CHECK: call @dec_free(%[[MV]]) : (!emitrust.struct<"dec">) -> ()

// RUN: split-file %s %t
// RUN: emitrust-import-c %t/local.c | FileCheck %s --check-prefix=LOCAL
// RUN: emitrust-import-c %t/member.c | FileCheck %s --check-prefix=MEMBER
// RUN: emitrust-import-c %t/indexing.c | FileCheck %s --check-prefix=INDEX

// FR-97: byte-splat memset over TYPED integer arrays — the
// generalization of FR-87's gated u32 member subset to every mapped
// width (i16/u16/i32/u32/i64/u64) and, new, to LOCAL typed arrays as
// destinations (FR-87 admitted member regions only; a local
// `int16_t last[256]` — heatshrink_encoder's do_indexing — died on the
// char-array rejection). memset semantics are bytes, so the fill is
// byte-exact exactly when the byte count provably covers whole
// elements (compile-time constant, multiple of the element size) and
// the fill byte is a compile-time constant: the fill WORD is then the
// byte replicated per width (b * 0x0101 / 0x01010101 /
// 0x0101010101010101, endianness-neutral because all bytes are equal).
// Pinned: the REPLICATION CONSTANTS for the non-symmetric byte 0xAB
// (0xABAB = -21589i16 / 43947u16, 0xABABABAB = -1414812757i32,
// 0xABAB..AB = -6076574518398440533i64) and for 0xFF (-1i16, u64's
// exact all-ones 18446744073709551615) — a hand-picked word constant
// is exactly the miscompile FR-87's byte-diff caught; the sizeof(a)
// count spelling (a C integer constant expression, folded by
// getIntegerConstantExpr — no new folding); LOCAL i16/u16/i32/u32/
// i64/u64 destinations; MEMBER i16/u16 destinations (arrow and dot
// roots) through the FR-74/86 interception; and the per-width
// `__emitrust_memset_*` helper images, whose `n` stays the BYTE count
// while the walker strides the element size.

//--- local.c
#include <string.h>
#include <stdint.h>

// LOCAL-LABEL: func.func @c_main
int main(void) {
  int16_t a[8];
  uint16_t b[8];
  int32_t q[4];
  unsigned int w[8];
  int64_t r[3];
  uint64_t u[2];
  /* i16 local, 0xFF fill, sizeof count: fill word 0xFFFF = -1i16. */
  // LOCAL: %[[AS:.+]] = emitrust.slice_of mut %{{.+}}[%{{.+}}] : (!emitrust.lvalue<!emitrust.array<8xi16>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i16>>
  // LOCAL: %[[AW:.+]] = arith.constant -1 : i16
  // LOCAL: emitrust.call_opaque "__emitrust_memset_i16"(%[[AS]], %[[AW]], %{{.+}}) : (!emitrust.mut_ref<!emitrust.slice<i16>>, i16, i64) -> ()
  memset(a, 0xFF, sizeof(a));
  /* i16 local, 0xAB (the non-symmetric proof byte): 0xABAB = -21589. */
  // LOCAL: %[[AB:.+]] = arith.constant -21589 : i16
  // LOCAL: emitrust.call_opaque "__emitrust_memset_i16"(%{{.+}}, %[[AB]], %{{.+}})
  memset(a, 0xAB, sizeof(a));
  /* i16 local, zero fill, PARTIAL count 4 (two elements). */
  // LOCAL: %[[AZ:.+]] = arith.constant 0 : i16
  // LOCAL: emitrust.call_opaque "__emitrust_memset_i16"(%{{.+}}, %[[AZ]], %{{.+}})
  memset(a, 0x00, 4);
  /* u16 local: the unsigned image, 0xABAB = 43947u16. */
  // LOCAL: %[[BS:.+]] = emitrust.slice_of mut %{{.+}}[%{{.+}}] : (!emitrust.lvalue<!emitrust.array<8xui16>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui16>>
  // LOCAL: %[[BW:.+]] = emitrust.constant <43947 : ui16> : ui16
  // LOCAL: emitrust.call_opaque "__emitrust_memset_u16"(%[[BS]], %[[BW]], %{{.+}}) : (!emitrust.mut_ref<!emitrust.slice<ui16>>, ui16, i64) -> ()
  memset(b, 0xAB, sizeof(b));
  /* i32 local: 0xABABABAB = -1414812757i32. */
  // LOCAL: %[[QW:.+]] = arith.constant -1414812757 : i32
  // LOCAL: emitrust.call_opaque "__emitrust_memset_i32"(%{{.+}}, %[[QW]], %{{.+}}) : (!emitrust.mut_ref<!emitrust.slice<i32>>, i32, i64) -> ()
  memset(q, 0xAB, sizeof(q));
  /* Local u32 array: FR-87's image now reachable from a local
     destination. */
  // LOCAL: %[[WW:.+]] = emitrust.constant <2880154539 : ui32> : ui32
  // LOCAL: emitrust.call_opaque "__emitrust_memset_u32"(%{{.+}}, %[[WW]], %{{.+}}) : (!emitrust.mut_ref<!emitrust.slice<ui32>>, ui32, i64) -> ()
  memset(w, 0xAB, sizeof(w));
  /* i64 local: 0xABAB..AB = -6076574518398440533i64. */
  // LOCAL: %[[RW:.+]] = arith.constant -6076574518398440533 : i64
  // LOCAL: emitrust.call_opaque "__emitrust_memset_i64"(%{{.+}}, %[[RW]], %{{.+}}) : (!emitrust.mut_ref<!emitrust.slice<i64>>, i64, i64) -> ()
  memset(r, 0xAB, sizeof(r));
  /* u64 local, 0xFF: the EXACT all-ones bit pattern (u64::MAX). */
  // LOCAL: %[[UW:.+]] = emitrust.constant <18446744073709551615 : ui64> : ui64
  // LOCAL: emitrust.call_opaque "__emitrust_memset_u64"(%{{.+}}, %[[UW]], %{{.+}}) : (!emitrust.mut_ref<!emitrust.slice<ui64>>, ui64, i64) -> ()
  memset(u, 0xFF, sizeof(u));
  return (int)(a[0] & 1) + (int)(b[0] & 1u) + (q[0] & 1) +
         (int)(w[0] & 1u) + (int)(r[0] & 1) + (int)(u[0] & 1u);
}

// Every requested per-width helper image is emitted exactly once, as
// safe Rust, in kStringHelpers table order; `n` stays the BYTE count.
// LOCAL: emitrust.verbatim "fn __emitrust_memset_u32(s: &mut [u32], w: u32, n: i64)
// LOCAL: emitrust.verbatim "fn __emitrust_memset_i16(s: &mut [i16], w: i16, n: i64)
// LOCAL: emitrust.verbatim "fn __emitrust_memset_u16(s: &mut [u16], w: u16, n: i64)
// LOCAL: emitrust.verbatim "fn __emitrust_memset_i32(s: &mut [i32], w: i32, n: i64)
// LOCAL: emitrust.verbatim "fn __emitrust_memset_i64(s: &mut [i64], w: i64, n: i64)
// LOCAL: emitrust.verbatim "fn __emitrust_memset_u64(s: &mut [u64], w: u64, n: i64)
// LOCAL-NOT: unsafe

//--- member.c
#include <string.h>
#include <stdint.h>

struct S {
  int16_t last[256];
  uint16_t w[4];
  unsigned int n;
};

// MEMBER-LABEL: func.func @touch(
// MEMBER-SAME: !emitrust.mut_ref<!emitrust.struct<"S">>
void touch(struct S *s) {
  /* ARROW root: i16 member array, 0xFF, sizeof count — the FR-74/86
     member interception feeds the same gated fill image as a local. */
  // MEMBER: %[[ML:.+]] = emitrust.member %{{.+}}["last"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.array<256xi16>>
  // MEMBER: %[[MS:.+]] = emitrust.slice_of mut %[[ML]][%{{.+}}] : (!emitrust.lvalue<!emitrust.array<256xi16>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i16>>
  // MEMBER: %[[MW:.+]] = arith.constant -1 : i16
  // MEMBER: emitrust.call_opaque "__emitrust_memset_i16"(%[[MS]], %[[MW]], %{{.+}}) : (!emitrust.mut_ref<!emitrust.slice<i16>>, i16, i64) -> ()
  memset(s->last, 0xFF, sizeof(s->last));
  /* ARROW root: u16 member, 0xFF = 65535u16. */
  // MEMBER: %[[WM:.+]] = emitrust.member %{{.+}}["w"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.array<4xui16>>
  // MEMBER: %[[WS:.+]] = emitrust.slice_of mut %[[WM]][%{{.+}}]
  // MEMBER: %[[WW:.+]] = emitrust.constant <65535 : ui16> : ui16
  // MEMBER: emitrust.call_opaque "__emitrust_memset_u16"(%[[WS]], %[[WW]], %{{.+}})
  memset(s->w, 0xFF, sizeof(s->w));
}

// MEMBER-LABEL: func.func @c_main
int main(void) {
  struct S s;
  /* DOT root over the local struct: 0xAB, partial count 8 (four
     elements of the 256). */
  // MEMBER: %[[DL:.+]] = emitrust.member %{{.+}}["last"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.array<256xi16>>
  // MEMBER: %[[DS:.+]] = emitrust.slice_of mut %[[DL]][%{{.+}}]
  // MEMBER: %[[DW:.+]] = arith.constant -21589 : i16
  // MEMBER: emitrust.call_opaque "__emitrust_memset_i16"(%[[DS]], %[[DW]], %{{.+}})
  memset(s.last, 0xAB, 8);
  s.n = 0u;
  touch(&s);
  return (int)(s.last[0] & 1);
}
// MEMBER: emitrust.verbatim "fn __emitrust_memset_i16(s: &mut [i16], w: i16, n: i64)
// MEMBER-NOT: unsafe

//--- indexing.c
#include <string.h>
#include <stdint.h>

/* heatshrink_encoder.c do_indexing, minimized (FR-96's residual
   micro-front): a LOCAL `int16_t last[256]` memset to 0xFF and read
   back through typed indexing. Library TU (no main). */
// INDEX-LABEL: func.func @probe
int16_t probe(void) {
  int16_t last[256];
  // INDEX: %[[PS:.+]] = emitrust.slice_of mut %{{.+}}[%{{.+}}] : (!emitrust.lvalue<!emitrust.array<256xi16>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i16>>
  // INDEX: %[[PW:.+]] = arith.constant -1 : i16
  // INDEX: emitrust.call_opaque "__emitrust_memset_i16"(%[[PS]], %[[PW]], %{{.+}})
  memset(last, 0xFF, sizeof(last));
  last[3] = 7;
  return last[3];
}
// INDEX: emitrust.verbatim "fn __emitrust_memset_i16(s: &mut [i16], w: i16, n: i64)
// INDEX-NOT: unsafe

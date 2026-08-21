// RUN: split-file %s %t
// RUN: emitrust-import-c %t/chain.c | FileCheck %s --check-prefix=CHAIN
// RUN: emitrust-import-c %t/subset.c | FileCheck %s --check-prefix=SUBSET
// RUN: emitrust-import-c %t/two.c | FileCheck %s --check-prefix=TWO
// RUN: emitrust-import-c %t/collide.c | FileCheck %s --check-prefix=COLLIDE
// RUN: emitrust-import-c %t/nest.c | FileCheck %s --check-prefix=NEST
// RUN: emitrust-import-c %t/recur.c | FileCheck %s --check-prefix=RECUR
// RUN: emitrust-import-c %t/nopass.c | FileCheck %s --check-prefix=NOPASS

// FR-101: BORROW-BUNDLE SCALARIZATION. The `output_info` idiom — a
// function-LOCAL struct whose members are BORROWS of the enclosing
// function's own parameters (`{ uint8_t *buf; size_t buf_size; size_t
// *output_size; }`, heatshrink_decoder.c:36-40), built once from those
// parameters and then passed BY ADDRESS down a chain of helpers that
// only project its members. The CTS-P2 static-binding model resolves a
// pointer member bound to a sibling local WITHIN one function, but it
// cannot carry that binding ACROSS a call, because the callee cannot
// name the caller's target object; Rust's answer would be a
// lifetime-parametric borrow struct, which is outside the region/cursor
// value model. So the answer is SROA on the C AST, ahead of every
// planner: the local instance and its member-binding stores are erased,
// each `B *` PARAMETER expands in place into one parameter per member,
// and every `&oi` / bare-`oi` argument expands into the member sources.
// Nothing downstream is bundle-aware — the residue lands on machinery
// that already exists (slice parameters for the byte member, FR-100
// callee-aware forwarding for the `size_t *output_size` member), which
// is why this increment adds no value kind, no dialect op and no
// lifetime model.
//
// This file pins the ADMITTED shapes. The gate's frontier arms (self-
// referential, escaping, wholesale copy, array/void*/struct* members,
// a member STORE through a `B *` parameter, and a bundle with no `B *`
// parameter at all) are pinned in borrow-bundle-scalarize-invalid.c.
//
// The LAST arm here (NOPASS) is a BYTE-IDENTITY pin, not a feature pin:
// a bundle-shaped struct whose address is never taken as a call
// argument has no `B *` parameter, so the transform must not fire and
// the existing CTS-P2 emission must survive unchanged. That confining
// clause is the reason this change cannot shift a byte of any emission
// that works today (its live twin is pointers-member.c's `struct Q`).

// THE 3-LEVEL CHAIN, heatshrink's exact shape: prototypes BEFORE the
// definitions, and the bundle parameter at index 0 (`can_take_byte`),
// index 1 (`add_tag_bit`) and LAST (`push_bits`) so the expansion is
// proven to happen IN PLACE at an arbitrary position.
//
// The record itself SURVIVES: scalarization makes it dead, but erasing
// it would shrink the FR-40 item graph, so it stays emitted.
// CHAIN: emitrust.struct_def @output_info ["buf", "buf_size", "output_size"] [i64, ui64, i64]
//
// The two member classes SPLIT in the same signature: `buf` is
// subscripted by this callee so it is a slice, `output_size` is only
// dereferenced so FR-100 keeps it a scalar reference, and `buf_size` is
// a plain value. The synthesized spellings are `<param>_<member>`.
// CHAIN-LABEL: func.func @push_bits(
// CHAIN-SAME: %[[PBUF:[^ ]+]]: !emitrust.mut_ref<!emitrust.slice<ui8>>, %{{[^ ]+}}: ui64, %[[POS:[^ ]+]]: !emitrust.mut_ref<ui64>)
// CHAIN-SAME: emitrust.param_names = ["st", "", "", "oi_buf", "", "oi_output_size"]
//
// One call, both classes, correctly split: the slice member is resliced
// at its cursor and the scalar member is forwarded as the bare
// reborrow (FR-100's arm, cashed here).
// CHAIN-LABEL: func.func @add_tag_bit(
// CHAIN-SAME: %{{[^ ]+}}: !emitrust.mut_ref<i32>, %{{[^ ]+}}: !emitrust.mut_ref<!emitrust.slice<ui8>>, %{{[^ ]+}}: ui64, %[[AOS:[^ ]+]]: !emitrust.mut_ref<ui64>, %{{[^ ]+}}: ui8)
// CHAIN: call @push_bits({{.*}}, %[[AOS]]) : (!emitrust.mut_ref<i32>, ui8, ui8, !emitrust.mut_ref<!emitrust.slice<ui8>>, ui64, !emitrust.mut_ref<ui64>) -> ()
//
// The builder function: no bundle instance, no member stores, and the
// call forwards the ENCLOSING PARAMETERS the members were bound to —
// substitution, not a per-member pointer local (materializing a pointer
// local for `output_size` would flip that parameter to a slice and
// break every caller of `poll`).
// CHAIN-LABEL: func.func @poll(
// CHAIN-SAME: %{{[^ ]+}}: !emitrust.mut_ref<!emitrust.slice<ui8>>, %{{[^ ]+}}: ui64, %[[LOS:[^ ]+]]: !emitrust.mut_ref<ui64>, %{{[^ ]+}}: i32)
// CHAIN-NOT: emitrust.struct<"output_info">
// CHAIN: call @yield_lit({{.*}}, %[[LOS]], {{.*}}) : (!emitrust.mut_ref<i32>, !emitrust.mut_ref<!emitrust.slice<ui8>>, ui64, !emitrust.mut_ref<ui64>, ui8) -> i32

//--- chain.c
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t *buf;
  size_t buf_size;
  size_t *output_size;
} output_info;

static int can_take_byte(output_info *oi);
static void add_tag_bit(int *st, output_info *oi, uint8_t tag);
static void push_bits(int *st, uint8_t count, uint8_t bits, output_info *oi);

static void push_bits(int *st, uint8_t count, uint8_t bits, output_info *oi) {
  uint8_t i;
  for (i = 0; i < count; i++) {
    if (*oi->output_size < oi->buf_size) {
      oi->buf[(*oi->output_size)++] = (uint8_t)(bits + i + (uint8_t)*st);
    }
  }
  *st += 1;
}
static void add_tag_bit(int *st, output_info *oi, uint8_t tag) {
  push_bits(st, 2, tag, oi);
}
static int can_take_byte(output_info *oi) {
  return *oi->output_size < oi->buf_size;
}
static int yield_lit(int *st, output_info *oi, uint8_t c) {
  if (can_take_byte(oi)) {
    add_tag_bit(st, oi, c);
    return 1;
  }
  return 0;
}
static void poll(uint8_t *out_buf, size_t out_buf_size, size_t *output_size,
                 int seed) {
  int st = seed;
  output_info oi;
  oi.buf = out_buf;
  oi.buf_size = out_buf_size;
  oi.output_size = output_size;
  int i;
  for (i = 0; i < 40; i++) {
    if (!yield_lit(&st, &oi, (uint8_t)(seed + i * 5))) { break; }
  }
}
int main(int argc, char **argv) {
  uint8_t buf[24];
  size_t n = 0;
  poll(buf, sizeof(buf), &n, argc);
  return (int)n;
}

// THE USED-MEMBER FIXPOINT. A `B *` parameter expands to only the
// members its callee TRANSITIVELY uses, computed as a call-graph
// fixpoint over the forwarding edges (the same shape as FR-100's
// `computeForwardSliceParams`). Expanding to ALL members instead makes
// a PANIC reachable on legal C: a callee that never touches the byte
// member would take a DEGENERATE `&mut u8`, whose caller-side reborrow
// `&mut oi_buf[0]` indexes an EMPTY slice on a zero-length output
// window — exactly heatshrink's `poll(hsd, &out[len], cap - len, &n)`
// when `len == cap`. `probe` here uses only `output_size`, so `buf` and
// `buf_size` must NOT appear in its signature at all; `write` uses all
// three. The exact-arity `-SAME` line below is the pin.
// SUBSET-LABEL: func.func @probe(
// SUBSET-SAME: %{{[^ ]+}}: !emitrust.mut_ref<ui64>) -> i32
// SUBSET-LABEL: func.func @write(
// SUBSET-SAME: %{{[^ ]+}}: !emitrust.mut_ref<!emitrust.slice<ui8>>, %{{[^ ]+}}: ui64, %{{[^ ]+}}: !emitrust.mut_ref<ui64>, %{{[^ ]+}}: ui8)
// The forwarder passes THREE arguments to `write` and ONE to `probe`,
// from the same expanded parameter list.
// SUBSET-LABEL: func.func @fwd(
// SUBSET: call @probe(%{{[^ ]+}}) : (!emitrust.mut_ref<ui64>) -> i32
// SUBSET: call @write({{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, ui64, !emitrust.mut_ref<ui64>, ui8) -> ()

//--- subset.c
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t buf_size; size_t *output_size; } oi_t;
static int probe(oi_t *oi) { return *oi->output_size < 8; }
static void write(oi_t *oi, uint8_t b) {
  if (*oi->output_size < oi->buf_size) { oi->buf[(*oi->output_size)++] = b; }
}
static void fwd(oi_t *oi, uint8_t b) {
  if (probe(oi)) { write(oi, b); }
}
static void drive(uint8_t *p, size_t n, size_t *o, int seed) {
  oi_t oi;
  oi.buf = p;
  oi.buf_size = n;
  oi.output_size = o;
  int i;
  for (i = 0; i < 4; i++) { fwd(&oi, (uint8_t)(seed + i)); }
}
int main(int argc, char **argv) {
  uint8_t b[8];
  size_t n = 0;
  drive(b, sizeof(b), &n, argc);
  return (int)n;
}

// TWO LIVE INSTANCES forwarded at TWO DIFFERENT `B *` parameters of the
// SAME call: each expands independently and in place, so `run` takes
// two full member triples and the arguments must not cross over.
// TWO-LABEL: func.func @run(
// TWO-SAME: %{{[^ ]+}}: !emitrust.mut_ref<!emitrust.slice<ui8>>, %{{[^ ]+}}: ui64, %{{[^ ]+}}: !emitrust.mut_ref<ui64>, %{{[^ ]+}}: !emitrust.mut_ref<!emitrust.slice<ui8>>, %{{[^ ]+}}: ui64, %{{[^ ]+}}: !emitrust.mut_ref<ui64>, %{{[^ ]+}}: i32)
// TWO-SAME: emitrust.param_names = ["a_buf", "", "a_output_size", "b_buf", "", "b_output_size", "n"]
// TWO-LABEL: func.func @drive(
// TWO-SAME: %{{[^ ]+}}: !emitrust.mut_ref<!emitrust.slice<ui8>>, %{{[^ ]+}}: ui64, %[[PO:[^ ]+]]: !emitrust.mut_ref<ui64>, %{{[^ ]+}}: !emitrust.mut_ref<!emitrust.slice<ui8>>, %{{[^ ]+}}: ui64, %[[QO:[^ ]+]]: !emitrust.mut_ref<ui64>, %{{[^ ]+}}: i32)
// TWO: call @run({{.*}}, %[[PO]], {{.*}}, %[[QO]], {{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, ui64, !emitrust.mut_ref<ui64>, !emitrust.mut_ref<!emitrust.slice<ui8>>, ui64, !emitrust.mut_ref<ui64>, i32) -> ()

//--- two.c
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t buf_size; size_t *output_size; } oi_t;
static void put(oi_t *oi, uint8_t b) {
  if (*oi->output_size < oi->buf_size) { oi->buf[(*oi->output_size)++] = b; }
}
static void run(oi_t *a, oi_t *b, int n) {
  int i;
  for (i = 0; i < n; i++) { put(a, (uint8_t)i); put(b, (uint8_t)(i * 2)); }
}
static void drive(uint8_t *p, size_t pn, size_t *po,
                  uint8_t *q, size_t qn, size_t *qo, int n) {
  oi_t x; x.buf = p; x.buf_size = pn; x.output_size = po;
  oi_t y; y.buf = q; y.buf_size = qn; y.output_size = qo;
  run(&x, &y, n);
}
int main(int argc, char **argv) {
  uint8_t a[6], b[6];
  size_t na = 0, nb = 0;
  drive(a, sizeof(a), &na, b, sizeof(b), &nb, argc + 2);
  return (int)(na + nb);
}

// NAME COLLISION: the callee already has a local spelled `oi_buf`, the
// name the expansion wants for its first member parameter. The
// synthesized spellings must be mangled against every name already in
// the function, and the collision must not silently capture the local.
// COLLIDE-LABEL: func.func @sink(
// COLLIDE-SAME: emitrust.param_names = ["oi_buf_", "", "oi_output_size", ""]
// COLLIDE: emitrust.variable named "oi_buf" : !emitrust.lvalue<ui64>

//--- collide.c
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t buf_size; size_t *output_size; } oi_t;
static void sink(oi_t *oi, uint8_t b) {
  size_t oi_buf = 3;
  oi_buf = oi_buf + b;
  if (*oi->output_size + oi_buf < oi->buf_size) {
    oi->buf[(*oi->output_size)++] = b;
  }
}
static void drive(uint8_t *p, size_t n, size_t *o, int seed) {
  oi_t oi;
  oi.buf = p;
  oi.buf_size = n;
  oi.output_size = o;
  sink(&oi, (uint8_t)seed);
}
int main(int argc, char **argv) {
  uint8_t b[8];
  size_t n = 0;
  drive(b, sizeof(b), &n, argc);
  return (int)n;
}

// A function that BOTH carries a `B *` parameter and BUILDS a second
// instance out of its own other parameters, plus a SELF-RECURSIVE
// forwarding edge. The first proves the two halves of the transform
// compose inside one body (the erased instance's arguments are drawn
// from `mid`'s parameters while `mid`'s own expanded parameters are
// forwarded in the same body); the second proves the used-member
// fixpoint is well defined on a cycle — a self edge adds nothing, so
// `rec` keeps exactly the members it projects.
// NEST-LABEL: func.func @mid(
// NEST-SAME: emitrust.param_names = ["oi_buf", "", "oi_output_size", "alt", "", "alto", ""]
// NEST: call @put({{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, ui64, !emitrust.mut_ref<ui64>, ui8) -> ()
// NEST: call @put({{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, ui64, !emitrust.mut_ref<ui64>, ui8) -> ()
// RECUR-LABEL: func.func @rec(
// RECUR-SAME: %[[RB:[^ ]+]]: !emitrust.mut_ref<!emitrust.slice<ui8>>, %[[RN:[^ ]+]]: ui64, %[[RO:[^ ]+]]: !emitrust.mut_ref<ui64>, %{{[^ ]+}}: ui8, %{{[^ ]+}}: i32)
// RECUR: call @rec({{.*}}, %[[RO]], {{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, ui64, !emitrust.mut_ref<ui64>, ui8, i32) -> ()

//--- nest.c
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t buf_size; size_t *output_size; } oi_t;
static void put(oi_t *oi, uint8_t b) {
  if (*oi->output_size < oi->buf_size) { oi->buf[(*oi->output_size)++] = b; }
}
static void mid(oi_t *oi, uint8_t *alt, size_t altn, size_t *alto, uint8_t b) {
  oi_t inner;
  inner.buf = alt;
  inner.buf_size = altn;
  inner.output_size = alto;
  put(&inner, (uint8_t)(b + 1));
  put(oi, b);
}
static void drive(uint8_t *p, size_t n, size_t *o, uint8_t *q, size_t qn,
                  size_t *qo, int seed) {
  oi_t oi;
  oi.buf = p;
  oi.buf_size = n;
  oi.output_size = o;
  int i;
  for (i = 0; i < 4; i++) { mid(&oi, q, qn, qo, (uint8_t)(seed + i)); }
}
int main(int argc, char **argv) {
  uint8_t a[8], b[8];
  size_t na = 0, nb = 0;
  drive(a, sizeof(a), &na, b, sizeof(b), &nb, argc);
  return (int)(na + nb);
}

//--- recur.c
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t buf_size; size_t *output_size; } oi_t;
static void rec(oi_t *oi, uint8_t b, int depth) {
  if (depth <= 0) { return; }
  if (*oi->output_size < oi->buf_size) { oi->buf[(*oi->output_size)++] = b; }
  rec(oi, (uint8_t)(b + 1), depth - 1);
}
static void drive(uint8_t *p, size_t n, size_t *o, int seed) {
  oi_t oi;
  oi.buf = p;
  oi.buf_size = n;
  oi.output_size = o;
  rec(&oi, (uint8_t)seed, 6);
}
int main(int argc, char **argv) {
  uint8_t b[4];
  size_t n = 0;
  drive(b, sizeof(b), &n, argc);
  return (int)n;
}

// BYTE-IDENTITY (the confining clause). `struct Q` is bundle-SHAPED —
// complete, non-self-referential, arithmetic-pointee members, a
// function local — but its address is never taken as a call argument,
// so no `Q *` parameter exists in the TU and the transform must not
// fire. The emission below is the pre-FR-101 CTS-P2 emission verbatim
// (its live twin is pointers-member.c's `deref_local`): the record is
// still emitted with its i64 cursor member, and `*q.ip` still resolves
// to `t`'s own place with no runtime state.
// NOPASS-LABEL: func.func @deref_local
// NOPASS: %[[T:.*]] = emitrust.variable named "t" : !emitrust.lvalue<i32>
// NOPASS: emitrust.assign %[[T]]
// NOPASS: %[[V:.*]] = emitrust.load %[[T]]
// NOPASS: %[[SUM:.*]] = arith.addi %[[V]]
// NOPASS: emitrust.assign %[[T]] = %[[SUM]]
// NOPASS: emitrust.struct_def @deref_local_Q ["ip", "pad"] [i64, i32]

//--- nopass.c
int deref_local(void) {
  int t = 41;
  struct Q { int *ip; int pad; } q;
  q.ip = &t;
  *q.ip = *q.ip + 1;
  return t;
}
int main(void) { return deref_local() - 42; }

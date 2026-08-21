// REQUIRES: cargo
// FR-101: differential end-to-end test for borrow-bundle scalarization
// — heatshrink's `output_info` idiom, which is the LAST shape standing
// on both heatshrink units. A function-LOCAL struct whose members are
// BORROWS of the enclosing function's own parameters is built once and
// then passed BY ADDRESS down a three-level chain of helpers that only
// project its members. The importer erases the instance and expands
// every `output_info *` parameter into one parameter per member, so the
// byte member becomes a slice parameter and the `size_t *` cursor
// member stays a scalar reference (FR-100) — IN THE SAME CALL.
//
// Only the stdout diff against the clang-built native can see this
// working. `cargo build` success proves nothing: a bundle expansion
// that crossed two members over, dropped a member's binding, or
// substituted the wrong enclosing parameter would still compile and
// would write different byte VALUES at different positions. Every
// stored value derives from argc and the loop bound derives from argc,
// so constant folding cannot pre-compute the fill.
//
// The `off` computation is the ZERO-LENGTH OUTPUT WINDOW regression
// arm, and it is why the expansion is used-member-aware. With no extra
// argv words the window is `&buf[24]` with size 0 — exactly
// heatshrink's `poll(hsd, &out[len], cap - len, &n)` when `len == cap`.
// If a `B *` parameter expanded to ALL members rather than only the
// members its callee transitively uses, `can_take_byte` (which never
// touches `buf`) would take a degenerate `&mut u8` and its caller would
// reborrow `&mut oi_buf[0]` — an index out of bounds panic on an empty
// slice, on legal C the native runs fine. That arm runs FIRST below.
//
// The bundle parameter sits at index 0 (`can_take_byte`), index 1
// (`add_tag_bit`) and LAST (`push_bits`), with the prototypes ahead of
// the definitions, so the in-place expansion is exercised at an
// arbitrary position and across a redeclaration.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/borrow_bundle_scalarize > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a > %t.native2.out
// RUN: %t.crate/target/release/borrow_bundle_scalarize a > %t.rust2.out
// RUN: diff %t.native2.out %t.rust2.out
// RUN: %t.native a b c > %t.native4.out
// RUN: %t.crate/target/release/borrow_bundle_scalarize a b c > %t.rust4.out
// RUN: diff %t.native4.out %t.rust4.out
// RUN: %t.native a b c d e f g > %t.native8.out
// RUN: %t.crate/target/release/borrow_bundle_scalarize a b c d e f g > %t.rust8.out
// RUN: diff %t.native8.out %t.rust8.out

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t *buf;         /* the byte member: its callee SUBSCRIPTS it */
  size_t buf_size;      /* a plain arithmetic member */
  size_t *output_size;  /* the cursor member: only ever DEREFERENCED */
} output_info;

/* Prototypes before the definitions, exactly as heatshrink writes them. */
static int can_take_byte(output_info *oi);
static void add_tag_bit(int *st, output_info *oi, uint8_t tag);
static void push_bits(int *st, uint8_t count, uint8_t bits, output_info *oi);

/* Level 3: the write idiom `oi->buf[(*oi->output_size)++] = c`. */
static void push_bits(int *st, uint8_t count, uint8_t bits, output_info *oi) {
  uint8_t i;
  for (i = 0; i < count; i++) {
    if (*oi->output_size < oi->buf_size) {
      oi->buf[(*oi->output_size)++] = (uint8_t)(bits + i + (uint8_t)*st);
    }
  }
  *st += 1;
}

/* Level 2: forwards the bundle from parameter index 1 to index 3. */
static void add_tag_bit(int *st, output_info *oi, uint8_t tag) {
  push_bits(st, 2, tag, oi);
}

/* Level 2b: the bundle at index 0, and its `buf` member is NEVER used —
   the used-member fixpoint must drop `buf` from this signature. */
static int can_take_byte(output_info *oi) {
  return *oi->output_size < oi->buf_size;
}

/* Level 1. */
static int yield_lit(int *st, output_info *oi, uint8_t c) {
  if (can_take_byte(oi)) {
    add_tag_bit(st, oi, c);
    return 1;
  }
  return 0;
}

/* Level 0: builds the bundle out of its own parameters. */
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
  size_t i;
  /* argc == 1 (no extra argv words) gives a ZERO-LENGTH window. */
  size_t off = (argc > 1) ? (size_t)0 : (size_t)24;
  for (i = 0; i < sizeof(buf); i++) { buf[i] = 0; }
  poll(&buf[off], sizeof(buf) - off, &n, argc);
  printf("n=%zu\n", (size_t)n);
  for (i = 0; i < n; i++) { printf("%d ", (int)buf[i]); }
  printf("\n");
  return 0;
}

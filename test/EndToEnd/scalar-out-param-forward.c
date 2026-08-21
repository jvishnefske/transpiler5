// REQUIRES: cargo
// FR-100: differential end-to-end test for callee-aware scalar
// out-parameter forwarding — the pervasive C idiom where a cursor
// (`size_t *output_size`) is threaded through a chain of helpers that
// only forward it, next to a byte buffer that its own callee
// SUBSCRIPTS. The importer must split those two apart in the SAME call:
// `output_size` keeps `&mut u64` (forwarded as the bare reborrow) while
// `out_buf` stays `&mut [u8]` (resliced at its cursor). If the
// forwarding chain lost track of which parameter is which, or if a
// reborrow aliased the wrong object, the emitted crate would write a
// different number of bytes or different byte VALUES — and only the
// stdout diff against the clang-built native can see that. `cargo
// build` success proves nothing here.
// The chain is three levels deep (fill -> can_take / push_byte, and
// fill -> bump1 -> bump2) so a demand that failed to propagate
// transitively would show up as either a rejection at import or a
// wrong cursor. Every stored value derives from argc, so constant
// folding cannot pre-compute the fill and hide a miscompile; the extra
// RUN pairs re-seed via additional argv words (argv itself is never
// read). Deterministic, all writes in bounds, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/scalar_out_param_forward > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a > %t.native2.out
// RUN: %t.crate/target/release/scalar_out_param_forward a > %t.rust2.out
// RUN: diff %t.native2.out %t.rust2.out
// RUN: %t.native a b c > %t.native4.out
// RUN: %t.crate/target/release/scalar_out_param_forward a b c > %t.rust4.out
// RUN: diff %t.native4.out %t.rust4.out

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

/* The buffer's callee SUBSCRIPTS it (slice) and the cursor's callee
   only dereferences it (scalar reference) — one call, two classes. */
static void push_byte(uint8_t *buf, size_t buf_size, size_t *output_size,
                      uint8_t byte) {
  buf[(*output_size)++] = byte;
}

/* Deref-only predicate on the forwarded cursor. */
static int can_take(size_t buf_size, size_t *output_size) {
  return *output_size < buf_size;
}

/* Two more forwarding levels, so the class must survive transitively. */
static void bump2(size_t *n, size_t by) { *n += by; }
static void bump1(size_t *n) { bump2(n, 2); }

static void fill(uint8_t *out_buf, size_t out_buf_size, size_t *output_size,
                 int seed) {
  int v = seed;
  while (can_take(out_buf_size, output_size)) {
    push_byte(out_buf, out_buf_size, output_size, (uint8_t)(v * 3 + 1));
    v++;
  }
  bump1(output_size);
}

int main(int argc, char **argv) {
  uint8_t buf[8];
  size_t n = 0;
  fill(buf, sizeof(buf), &n, argc);
  printf("n=%zu\n", n);
  for (size_t i = 0; i < sizeof(buf); i++)
    printf("%u ", buf[i]);
  printf("\n");
  return 0;
}

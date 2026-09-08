// REQUIRES: cargo
// FR-208: THE oracle for "the exported symbol is the one a C caller actually
// links against". `--c-abi-exports` exists to promise a bare symbol to dlsym,
// but `#[no_mangle]`/`#[export_name]` were written from the MLIR symbol -- and
// the MLIR symbol is the FR-53 IDIOMATIC RENAME, not the C spelling. So a C
// program with any non-snake_case name got a shared object exporting a name it
// never had: measured `nm -D` carrying `T spx_add` against a C host's
// `undefined reference to 'SPX_add'`.
//
// Reading the emitted text cannot pin this. A CHECK line asserting
// `#[export_name = "SPX_add"]` is satisfied by a string the linker never sees;
// only a real dynamic symbol proves the contract. So this test builds the
// emitted crate as a real cdylib and dlopens it from a clang-built C host that
// dlsyms every entry BY ITS C SPELLING, calls it through the C signature, and
// byte-diffs the result against the clang-built native of the same source. The
// host additionally asserts the renamed spellings are ABSENT, so a future
// change that exported both names would not pass by accident.
//
// Every symbol here is chosen so the test FAILS AT THE PRE-FR-208 EMITTER, and
// the two emission paths the defect lived in are both covered:
//   * CLASS 0 (`#[no_mangle]` on the function itself) -- `SPX_add`,
//     `SPX_crcStep`, `spxWiden`, `SPX_blend`, `SPX_mix8`. The three casings
//     are deliberate: an UPPER_ prefix, an interior camel hump, and a
//     leading-lowercase camel name each fold differently under `toSnakeCase`;
//   * the WRAPPER path (`#[export_name]` on a generated `unsafe extern "C"`
//     item) -- `SPX_pair_sum` and `SPX_pair_scale`, FR-182 class 1, whose
//     `spx_pair` storage the HOST allocates so the `#[repr(C)]` promise is
//     exercised at the same time;
//   * `plain_add` is the CONTROL. Its C spelling is already snake_case, so it
//     must keep the historical `#[no_mangle]` and its bytes must not move --
//     the fix has to be targeted, not a blanket switch to `#[export_name]`.
// `SPX_mix8` has EIGHT integer parameters so the last two travel on the stack,
// and `SPX_blend` mixes `double` with `float`: a wrapper that got the NAME
// right and the signature wrong still shows up in the diff. Every input
// derives from argc (via the shared driver's `seed`), so constant folding
// cannot pre-compute an answer on either side, and the second RUN pair
// re-seeds through extra argv words.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports \
// RUN:   --crate-name=cabimixed %s -o %t.crate --build
// RUN: clang -std=c11 -DC_ABI_NATIVE_MAIN %s -o %t.native
// RUN: clang -std=c11 %S/Inputs/c-abi-exports-mixed-case-host.c -o %t.host -ldl
//
// The crate root really carries the C spellings, and the already-snake_case
// control really kept `#[no_mangle]` -- checked before the run so an emission
// regression is reported as itself rather than as a dlsym failure.
// RUN: cat %t.crate/src/lib.rs | FileCheck %s --check-prefix=LIB
//
// RUN: %t.native > %t.native.out
// RUN: %t.host %t.crate/target/release/libcabimixed%shlibext > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.host %t.crate/target/release/libcabimixed%shlibext a b \
// RUN:   > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out
//
// FR-208, the `--preserve-c-names` half: that mode already emitted the right
// symbols (it is how the defect was worked around before the fix), so it must
// become a NO-OP for the export name -- not a second, divergent naming rule.
// The SAME host, dlsyming the SAME C spellings, is what proves it: the crate
// is emitted again with the whole-crate mode switch on and its output must be
// byte-identical to the idiomatic crate's.
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports \
// RUN:   --preserve-c-names --crate-name=cabimixedpcn %s -o %t.pcn --build
// RUN: cat %t.pcn/src/lib.rs | FileCheck %s --check-prefix=PCN
// RUN: %t.host %t.pcn/target/release/libcabimixedpcn%shlibext > %t.pcn.out
// RUN: diff %t.native.out %t.pcn.out
// RUN: %t.host %t.pcn/target/release/libcabimixedpcn%shlibext a b \
// RUN:   > %t.pcn3.out
// RUN: diff %t.native3.out %t.pcn3.out

#include "Inputs/c-abi-exports-mixed-case-types.h"

/* CLASS 0, UPPER_ prefix: `SPX_add` folds to `spx_add`. */
int SPX_add(int a, int b) { return a * 3 + b; }

/* CLASS 0, interior camel hump: `SPX_crcStep` folds to `spx_crc_step`. */
unsigned SPX_crcStep(unsigned crc, unsigned char b) {
  unsigned i;
  crc = crc ^ (unsigned)b;
  for (i = 0; i < 8; ++i) {
    if (crc & 1u)
      crc = (crc >> 1) ^ 0xA001u;
    else
      crc = crc >> 1;
  }
  return crc;
}

/* CLASS 0, leading-lowercase camel: `spxWiden` folds to `spx_widen`. */
long long spxWiden(int lo, int hi) {
  return ((long long)hi << 32) | (long long)(unsigned)lo;
}

/* CLASS 0, two float classes. */
double SPX_blend(double a, float b) { return a + (double)b; }

/* CLASS 0, eight integer parameters: the seventh and eighth travel on the
   stack, where a shifted argument list first becomes visible. */
int SPX_mix8(int a, int b, int c, int d, int e, int f, int g, int h) {
  return a + b * 2 + c * 4 + d * 8 + e * 16 + f * 32 + g * 64 + h * 128;
}

/* THE CONTROL: already snake_case, so nothing about it may change. */
int plain_add(int a, int b) { return a - b; }

/* The WRAPPER path (FR-182 class 1): a pointer to an ABI-faithful record. */
int SPX_pair_sum(spx_pair *p) { return p->lo * 100 + p->hi; }

void SPX_pair_scale(spx_pair *p, int k) {
  p->lo = p->lo * k;
  p->hi = p->hi + k;
}

// The native oracle's entry point, compiled only for the native leg. It runs
// the SAME driver body the dlopen host runs, so the two outputs are comparable
// by construction.
#ifdef C_ABI_NATIVE_MAIN
#include <stdio.h>
#include "Inputs/c-abi-exports-mixed-case-driver.h"

int main(int argc, char **argv) {
  (void)argv;
  run_driver(argc);
  return 0;
}
#endif

// The C spelling is what `#[export_name]` binds, while the ITEM keeps the
// idiomatic rename -- so every internal call site and every other byte of the
// crate is exactly what it was before FR-208.
// LIB:      #[export_name = "SPX_add"]
// LIB-NEXT: pub extern "C" fn spx_add(
// LIB:      #[export_name = "SPX_crcStep"]
// LIB-NEXT: pub extern "C" fn spx_crc_step(
// LIB:      #[export_name = "spxWiden"]
// LIB-NEXT: pub extern "C" fn spx_widen(
// LIB:      #[export_name = "SPX_blend"]
// LIB-NEXT: pub extern "C" fn spx_blend(
// LIB:      #[export_name = "SPX_mix8"]
// LIB-NEXT: pub extern "C" fn spx_mix8(
//
// The control keeps `#[no_mangle]`: when the two spellings agree there is
// nothing to rewrite, which is what keeps every already-snake_case crate
// byte-identical.
// LIB:      #[no_mangle]
// LIB-NEXT: pub extern "C" fn plain_add(
//
// The wrapper path: the generated item is still named for the emitted symbol
// (it cannot take the function's own name -- rustc E0428), and only the
// exported STRING moved to the C spelling.
// LIB:      #[export_name = "SPX_pair_sum"]
// LIB-NEXT: pub unsafe extern "C" fn __emitrust_cabi_spx_pair_sum(
// LIB:      #[export_name = "SPX_pair_scale"]
// LIB-NEXT: pub unsafe extern "C" fn __emitrust_cabi_spx_pair_scale(

// Under `--preserve-c-names` the ITEM is already spelled with the C name, so
// the export needs no redirection and the historical `#[no_mangle]` is exact.
// Same exported symbols, reached a different way -- which is precisely what
// "the flag is a no-op for the export name" means.
// PCN:      #[no_mangle]
// PCN-NEXT: pub extern "C" fn SPX_add(
// PCN:      #[no_mangle]
// PCN-NEXT: pub extern "C" fn SPX_crcStep(
// PCN:      #[no_mangle]
// PCN-NEXT: pub extern "C" fn spxWiden(
// PCN:      #[no_mangle]
// PCN-NEXT: pub extern "C" fn SPX_blend(
// PCN:      #[no_mangle]
// PCN-NEXT: pub extern "C" fn SPX_mix8(
// PCN:      #[no_mangle]
// PCN-NEXT: pub extern "C" fn plain_add(
// PCN:      #[export_name = "SPX_pair_sum"]
// PCN-NEXT: pub unsafe extern "C" fn __emitrust_cabi_SPX_pair_sum(

// REQUIRES: cargo
// FR-202: THE oracle for the proven-bound slice export. `cargo build`
// succeeding proves exactly NOTHING here, and this is the feature for which
// that statement is most literally true: FR-181 measured that exporting a
// `&[u8]` parameter across `extern "C"` compiles with only a "uses type [u8],
// which is not FFI-safe" WARNING and then reads the slice length out of the
// caller's NEXT argument -- native 21983 against export 25322 on crc16, exit
// 0, no panic, no diagnostic. Only a byte-diff against the clang native sees
// that class at all.
//
// So this test builds the emitted crate as a real cdylib, dlopens the produced
// shared object from a C host that knows nothing but the C ABI, dlsyms each
// BARE symbol, calls it through the ORIGINAL C signature -- one pointer
// argument slot, no length beside it -- and byte-diffs the result against the
// clang-built native of the same C source.
//
// The functions are chosen so no bound or slot mistake can hide:
//   * `hdr_bitrate` is the TRACTOR corpus function verbatim
//     (Public-Tests/B01_organic/hdr_bitrate_lib). It reads h[1] and h[2], the
//     bound is 3, and the corpus's own runner hands it a `[u8; 3]`;
//   * `byte_pick(p, k)` puts a SCALAR AFTER the pointer -- FR-181's measured
//     shift, where `k` would receive the synthesized length;
//   * `lead_scalar(k, p)` puts one BEFORE it, so the reference is not required
//     to be parameter zero;
//   * `bump_read` reads index 3 of a FOUR-byte buffer after a `p += 2`, so a
//     bound computed one too small is an index panic in a function that cannot
//     unwind -- SIGABRT, not a quiet wrong number.
// Every buffer the driver allocates is EXACTLY the proven bound and not a byte
// more, and every value derives from argc (via the shared driver's `seed`), so
// constant folding cannot pre-compute an answer on either side. The second RUN
// pair re-seeds through extra argv words.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports \
// RUN:   --crate-name=cabislice %s -o %t.crate --build
// RUN: clang -std=c11 -DC_ABI_NATIVE_MAIN %s -o %t.native
// RUN: clang -std=c11 %S/Inputs/c-abi-exports-slice-dlopen-host.c \
// RUN:   -o %t.host -ldl
//
// The manifest really asked for a shared object, and the crate root really
// carries a wrapper per export with a LITERAL length -- checked before the run
// so a packaging or gating regression is reported as itself rather than as a
// dlsym failure.
// RUN: cat %t.crate/Cargo.toml | FileCheck %s --check-prefix=TOML
// RUN: cat %t.crate/src/lib.rs | FileCheck %s --check-prefix=LIB
//
// RUN: %t.native > %t.native.out
// RUN: %t.host %t.crate/target/release/libcabislice%shlibext > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.host %t.crate/target/release/libcabislice%shlibext a b \
// RUN:   > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

/* The corpus function, verbatim. */
unsigned hdr_bitrate(const unsigned char *h) {
  static const unsigned char halfrate[2][3][15] = {
      {{0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80},
       {0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80},
       {0, 16, 24, 28, 32, 40, 48, 56, 64, 72, 80, 88, 96, 112, 128}},
      {{0, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160},
       {0, 16, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192},
       {0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224}},
  };
  return 2 *
         halfrate[!!((h[1]) & 0x8)][(((h[1]) >> 1) & 3) - 1][((h[2]) >> 4)];
}

/* A scalar AFTER the pointer: `k` must keep its own argument slot. */
int byte_pick(const unsigned char *p, int k) { return p[0] * k + p[1]; }

/* A scalar BEFORE it. */
int lead_scalar(int k, const unsigned char *p) { return k * 100 + p[0] + p[1]; }

/* Pointer arithmetic: the highest index actually reached is 3, so the bound is
   4 and the driver's buffer is four bytes. */
unsigned bump_read(const unsigned char *p) {
  p += 2;
  return p[1];
}

// The native oracle's entry point, compiled only for the native leg. It runs
// the SAME driver body the dlopen host runs, so the two outputs are comparable
// by construction.
#ifdef C_ABI_NATIVE_MAIN
#include <stdio.h>
#include "Inputs/c-abi-exports-slice-dlopen-driver.h"

int main(int argc, char **argv) {
  (void)argv;
  run_driver(argc);
  return 0;
}
#endif

// TOML:      [lib]
// TOML-NEXT: name = "cabislice"
// TOML-NEXT: path = "src/lib.rs"
// TOML-NEXT: crate-type = ["cdylib"]

// Four wrappers, four literal lengths, and not one `extern "C"` on a
// translated function: the `&[u8]` in a `pub fn` signature is not C's pointer,
// and giving it the C ABI is the FR-138 mismatch this feature exists to avoid.
// LIB: #[export_name = "hdr_bitrate"]
// LIB-NEXT: pub unsafe extern "C" fn __emitrust_cabi_hdr_bitrate(h: *const u8) -> u32 {
// LIB-NEXT:     hdr_bitrate(core::slice::from_raw_parts(h, 3))
// LIB: #[export_name = "byte_pick"]
// LIB-NEXT: pub unsafe extern "C" fn __emitrust_cabi_byte_pick(p: *const u8, k: i32) -> i32 {
// LIB-NEXT:     byte_pick(core::slice::from_raw_parts(p, 2), k)
// LIB: #[export_name = "lead_scalar"]
// LIB-NEXT: pub unsafe extern "C" fn __emitrust_cabi_lead_scalar(k: i32, p: *const u8) -> i32 {
// LIB-NEXT:     lead_scalar(k, core::slice::from_raw_parts(p, 2))
// LIB: #[export_name = "bump_read"]
// LIB-NEXT: pub unsafe extern "C" fn __emitrust_cabi_bump_read(p: *const u8) -> u32 {
// LIB-NEXT:     bump_read(core::slice::from_raw_parts(p, 4))

// REQUIRES: cargo
// FR-139: THE oracle for `--c-abi-exports`. No existing test in this suite
// crosses an FFI boundary, and `cargo build` succeeding proves exactly nothing
// about one: rustc compiles an `extern "C"` signature mismatch with at most a
// warning and the wrong answer only appears at the call. So this test builds
// the emitted crate as a real cdylib, dlopens the produced shared object from
// a C host that knows nothing but the C ABI, dlsyms each BARE symbol, calls it
// through the C signature, and byte-diffs the result against the clang-built
// native of the same C source.
//
// The library below is all-scalar on purpose -- that is the entire safe subset
// FR-139 claims. Its signatures are chosen to make an ABI mismatch impossible
// to hide:
//   * `scale`/`mix6` fill and then exhaust nothing; `mix8` has EIGHT integer
//     parameters, so the last two are passed on the STACK, which is where a
//     shifted argument list first shows up;
//   * `mixfd` interleaves integer and floating-point parameters, so the two
//     register classes must be assigned independently and in order;
//   * `blend` mixes `double` and `float`, `widen` returns a 64-bit value from
//     two 32-bit ones, and `crc_step` takes a `u8` beside a `u32`. A widened,
//     narrowed or transposed argument changes the printed bytes.
// Every input derives from argc (via the shared driver's `seed`), so constant
// folding cannot pre-compute an answer on either side, and the second RUN pair
// re-seeds through extra argv words. Deterministic, no UB: the float values
// are exact binary fractions and the CRC arithmetic is unsigned.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports \
// RUN:   --crate-name=cabidlopen %s -o %t.crate --build
// RUN: clang -std=c11 -DC_ABI_NATIVE_MAIN %s -o %t.native
// RUN: clang -std=c11 %S/Inputs/c-abi-exports-dlopen-host.c -o %t.host -ldl
//
// The manifest really asked for a shared object, and the crate root really
// exports bare symbols -- checked before the run so a packaging regression is
// reported as itself rather than as a dlsym failure.
// RUN: cat %t.crate/Cargo.toml | FileCheck %s --check-prefix=TOML
// RUN: cat %t.crate/src/lib.rs | FileCheck %s --check-prefix=LIB
//
// RUN: %t.native > %t.native.out
// RUN: %t.host %t.crate/target/release/libcabidlopen%shlibext > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.host %t.crate/target/release/libcabidlopen%shlibext a b \
// RUN:   > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

int scale(int v, int k) { return v * k; }

unsigned crc_step(unsigned crc, unsigned char b) {
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

long long widen(int lo, int hi) {
  return ((long long)hi << 32) | (long long)(unsigned)lo;
}

double blend(double a, float b) { return a + (double)b; }

int mix6(int a, int b, int c, int d, int e, int f) {
  return a + b * 10 + c * 100 + d * 1000 + e * 10000 + f * 100000;
}

/* Eight integer parameters: on the SysV x86-64 C ABI the seventh and eighth
   travel on the stack, so a shifted or fattened earlier parameter corrupts
   these two first. */
int mix8(int a, int b, int c, int d, int e, int f, int g, int h) {
  return a + b * 2 + c * 4 + d * 8 + e * 16 + f * 32 + g * 64 + h * 128;
}

/* Interleaved classes: a,c,e take integer registers and b,d take SSE ones,
   each sequence independent of the other. */
double mixfd(int a, double b, int c, float d, int e) {
  return (double)a * b + (double)c - (double)d + (double)e;
}

// The native oracle's entry point, compiled only for the native leg. It runs
// the SAME driver body the dlopen host runs, so the two outputs are comparable
// by construction.
#ifdef C_ABI_NATIVE_MAIN
#include <stdio.h>
#include "Inputs/c-abi-exports-dlopen-driver.h"

int main(int argc, char **argv) {
  (void)argv;
  run_driver(argc);
  return 0;
}
#endif

// TOML:      [lib]
// TOML-NEXT: name = "cabidlopen"
// TOML-NEXT: path = "src/lib.rs"
// TOML-NEXT: crate-type = ["cdylib"]

// LIB: #[no_mangle]
// LIB: pub extern "C" fn scale(
// LIB: #[no_mangle]
// LIB: pub extern "C" fn crc_step(
// LIB: #[no_mangle]
// LIB: pub extern "C" fn widen(
// LIB: #[no_mangle]
// LIB: pub extern "C" fn blend(
// LIB: #[no_mangle]
// LIB: pub extern "C" fn mix6(
// LIB: #[no_mangle]
// LIB: pub extern "C" fn mix8(
// LIB: #[no_mangle]
// LIB: pub extern "C" fn mixfd(

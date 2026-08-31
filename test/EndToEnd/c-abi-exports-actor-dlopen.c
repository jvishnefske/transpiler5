// REQUIRES: cargo
// FR-179 (FR-139 follow-on): THE oracle for the actor-lift C-ABI wrapper.
//
// `cargo build` succeeding proves NOTHING here. The whole question is whether
// a bare C symbol exists at all and whether the state behind it is the same
// state on the second call as on the first, and both of those are invisible to
// the compiler. So this test builds the emitted crate as a real cdylib,
// dlopens the shared object from a C host that knows only the C ABI, dlsyms
// each bare symbol, calls it through the C signature, and byte-diffs the
// result against the clang-built native of this same source.
//
// Every function below is all-scalar in C and every one of them is turned into
// a `&mut self` method by FR-62's actor lift, because each touches file-scope
// state. Before the wrapper existed this crate exported ZERO symbols and every
// dlsym in the host returned null.
//
// The state is chosen so a wrong singleton cannot hide:
//   * `encode` reads TWO lifted tables, so an owner that was never constructed
//     (or constructed with `Default` instead of the C initializers) prints
//     zeros where the native prints table entries;
//   * `acc_add` and `acc_get` are two wrappers on ONE owner, and the shared
//     driver interleaves them. A per-call `Actor::new()` agrees on call 1 and
//     diverges on call 2; two singletons for two wrappers make `acc_get`
//     report zero forever. Both show up as a diff, not as a warning.
// Every input derives from argc via the shared driver's `seed`, so no result
// can be constant-folded on either side, and the second RUN pair re-seeds
// through extra argv words. Deterministic and no UB: the arithmetic is on
// small ints and the table indices are masked.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports \
// RUN:   --crate-name=cabiactor %s -o %t.crate --build
// RUN: clang -std=c11 -DC_ABI_NATIVE_MAIN %s -o %t.native
// RUN: clang -std=c11 %S/Inputs/c-abi-exports-actor-host.c -o %t.host -ldl
//
// The crate really is a cdylib and really does carry the wrappers -- checked
// before the run so a packaging or emission regression reports as itself
// rather than as a dlsym failure.
// RUN: cat %t.crate/Cargo.toml | FileCheck %s --check-prefix=TOML
// RUN: cat %t.crate/src/lib.rs | FileCheck %s --check-prefix=LIB
//
// RUN: %t.native > %t.native.out
// RUN: %t.host %t.crate/target/release/libcabiactor%shlibext > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.host %t.crate/target/release/libcabiactor%shlibext a b \
// RUN:   > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

/* Two lifted tables read by one function: an owner that never ran its
   synthesized `new()` would read a `Default` (all-zero) copy of these. */
static unsigned short enc_base[16] = {3,   5,   7,   11,  13,  17,  19,  23,
                                      29,  31,  37,  41,  43,  47,  53,  59};
static unsigned char enc_shift[16] = {1, 2, 3, 4, 1, 2, 3, 4,
                                      5, 6, 7, 8, 5, 6, 7, 8};

/* Mutated lifted state reached by TWO exported functions. */
static int acc;

unsigned short encode(int i) {
  int slot = i & 15;
  return (unsigned short)(enc_base[slot] << enc_shift[slot]);
}

int acc_add(int d) {
  acc = acc + d;
  return acc;
}

int acc_get(void) { return acc; }

// The native oracle's entry point, compiled only for the native leg. It runs
// the SAME driver body the dlopen host runs, so the two outputs are comparable
// by construction.
#ifdef C_ABI_NATIVE_MAIN
#include <stdio.h>
#include "Inputs/c-abi-exports-actor-driver.h"

int main(int argc, char **argv) {
  (void)argv;
  run_driver(argc);
  return 0;
}
#endif

// TOML:      [lib]
// TOML-NEXT: name = "cabiactor"
// TOML-NEXT: path = "src/lib.rs"
// TOML-NEXT: crate-type = ["cdylib"]

// ONE singleton for the two-function owner, and one wrapper per exported
// function -- the methods themselves keep their lifted `&mut self` shape.
// LIB: pub fn encode(&mut self, i: i32) -> u16 {
// LIB: pub fn acc_add(&mut self, d: i32) -> i32 {
// LIB: pub fn acc_get(&mut self) -> i32 {
// LIB: thread_local! {
// LIB: #[no_mangle]
// LIB: pub extern "C" fn encode(i: i32) -> u16 {
// LIB: #[no_mangle]
// LIB: pub extern "C" fn acc_add(d: i32) -> i32 {
// LIB: #[no_mangle]
// LIB: pub extern "C" fn acc_get() -> i32 {

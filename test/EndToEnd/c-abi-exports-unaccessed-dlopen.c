// REQUIRES: cargo
// FR-226: THE oracle for the unaccessed-pointer C-ABI export. `cargo build`
// succeeding proves exactly NOTHING here. The failure mode this class could
// have -- a wrapper that quietly builds a real slice out of the caller's
// pointer, or that lets the fat pointer's length eat an argument register --
// is FR-181's measured class: exit 0, no panic, no diagnostic, only wrong
// numbers. Only a byte-diff against the clang-built native sees it, and a NULL
// argument turns the worst variant into a SIGSEGV instead of a silent one.
//
// WHY THIS CLASS EXISTS AT ALL. The TRACTOR corpus's
// `SPX_initialize_hash_function` is, in its blake configurations, literally
// `void initialize_hash_function(spx_ctx *ctx) { (void)ctx; }`. It imports
// faithfully and then had no C-ABI export, because FR-182 class 1 needs a
// struct REFERENCE (a C `T *` is imported as a SLICE) and FR-202 class 2 needs
// a MUST-ACCESS BOUND proven from a body that accesses nothing. Twelve corpus
// cases scored SYMBOL_MISSING on the one function in the crate that is
// trivially the safest thing to export.
//
// WHAT IS ACTUALLY UNDER TEST, and it is a promise about MEMORY, not about a
// return value: the exported symbol must never touch the pointer it is given.
// So the host allocates a real `spx_ctx` with the C compiler's layout, fills
// all 36 of its bytes from the seed, hands its address across, and then prints
// every one of them back. It also calls the same symbol with NULL -- the exact
// case `cAbiProvenSliceBound`'s refused-zero-bound comment worries about --
// and with a wild but correctly aligned address that no mapping backs. A
// wrapper that dereferenced, or that called `from_raw_parts(p, 0)`, cannot
// survive those two lines.
//
// `trail` and `lead` are the argument-slot oracles: a seed-derived scalar
// after and before the pointer, so that if the slice were ever exported
// directly, the scalar would receive the synthesized length instead (FR-181,
// measured on crc16 as native 21983 against export 25322).
//
// Every value derives from argc (via the shared driver's `seed`), so constant
// folding cannot pre-compute an answer on either side, and the second RUN pair
// re-seeds through extra argv words.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports \
// RUN:   --crate-name=cabinoaccess %s -o %t.crate --build
// RUN: clang -std=c11 -DC_ABI_NATIVE_MAIN %s -o %t.native
// RUN: clang -std=c11 %S/Inputs/c-abi-exports-unaccessed-host.c \
// RUN:   -o %t.host -ldl
//
// The manifest really asked for a shared object, and the crate root really
// carries a SAFE wrapper per export with an OPAQUE pointee -- checked before
// the run so a packaging or gating regression is reported as itself rather
// than as a dlsym failure.
// RUN: cat %t.crate/Cargo.toml | FileCheck %s --check-prefix=TOML
// RUN: cat %t.crate/src/lib.rs | FileCheck %s --check-prefix=LIB
//
// The zero-`unsafe` claim is a property of this class, not a convenience: the
// wrapper performs no unsafe operation, so the whole emitted crate has none.
// RUN: not grep unsafe %t.crate/src/lib.rs
// RUN: not grep from_raw_parts %t.crate/src/lib.rs
//
// RUN: %t.native > %t.native.out
// RUN: %t.host %t.crate/target/release/libcabinoaccess%shlibext > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.host %t.crate/target/release/libcabinoaccess%shlibext a b \
// RUN:   > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include "Inputs/c-abi-exports-unaccessed-types.h"

/* The corpus function, verbatim modulo the record's name. */
void initialize_hash_function(spx_ctx *ctx) { (void)ctx; }

/* The SHARED half: a `const unsigned char *` the body never reads. */
int untouched(const unsigned char *p) {
  (void)p;
  return 7;
}

/* A scalar AFTER the pointer: `k` must keep its own argument slot. */
int trail(unsigned char *p, int k) {
  (void)p;
  return k + 11;
}

/* A scalar BEFORE it. */
int lead(int k, unsigned char *p) {
  (void)p;
  return k * 3;
}

// The native oracle's entry point, compiled only for the native leg. It runs
// the SAME driver body the dlopen host runs, so the two outputs are comparable
// by construction.
#ifdef C_ABI_NATIVE_MAIN
#include "Inputs/c-abi-exports-unaccessed-driver.h"

int main(int argc, char **argv) {
  (void)argv;
  run_driver(argc);
  return 0;
}
#endif

// TOML:      [lib]
// TOML-NEXT: name = "cabinoaccess"
// TOML-NEXT: path = "src/lib.rs"
// TOML-NEXT: crate-type = ["cdylib"]

// Four wrappers, every one of them SAFE, every pointee OPAQUE, and every
// argument an EMPTY slice the wrapper owns. Not one `extern "C"` on a
// translated function: the `&mut [T]` in a `pub fn` signature is not C's
// pointer, and giving it the C ABI is the FR-138 mismatch this feature exists
// to avoid.
// LIB: #[export_name = "initialize_hash_function"]
// LIB-NEXT: pub extern "C" fn __emitrust_cabi_initialize_hash_function(_ctx: *mut core::ffi::c_void) {
// LIB-NEXT:     initialize_hash_function(&mut [])
// LIB: #[export_name = "untouched"]
// LIB-NEXT: pub extern "C" fn __emitrust_cabi_untouched(_p: *const core::ffi::c_void) -> i32 {
// LIB-NEXT:     untouched(&[])
// LIB: #[export_name = "trail"]
// LIB-NEXT: pub extern "C" fn __emitrust_cabi_trail(_p: *mut core::ffi::c_void, k: i32) -> i32 {
// LIB-NEXT:     trail(&mut [], k)
// LIB: #[export_name = "lead"]
// LIB-NEXT: pub extern "C" fn __emitrust_cabi_lead(k: i32, _p: *mut core::ffi::c_void) -> i32 {
// LIB-NEXT:     lead(k, &mut [])

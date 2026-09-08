// FR-202 (the last residue of FR-181): a `const unsigned char *` parameter --
// which the importer models as the SHARED byte slice `&[u8]` -- gets a C-ABI
// export when, and only when, the callee's MUST-ACCESS BOUND is PROVEN from
// its body.
//
// Why a bound is needed at all, and why it cannot be guessed. FR-181 measured
// that `&[T]` is a TWO-register fat pointer: exporting `extern "C" fn(d: &[u8],
// a: u32, b: u16)` and calling it from C as `probe(buf, 9, 2000)` makes Rust
// see `ptr, len=9, a=2000, b=0` -- every later argument shifts one slot and the
// length is invented out of the caller's next argument. On crc16 that is
// native 21983 against export 25322, exit 0, no panic, no diagnostic. FR-181
// also killed the obvious repair ("the integer after a pointer is its length")
// with two counterexamples from inside the corpus: `synth_pair`'s `nch` is a
// channel COUNT whose real bound is `16*nch+1`, and `wcscat`'s `numElem` is a
// CAPACITY that its own vector 5 has smaller than the string.
//
// So the length is never taken from the signature. It is PROVEN from the body,
// and the wrapper -- not the translated function -- is what carries the C
// symbol: it takes the raw `*const u8` C really passes (ONE register, so no
// argument shifts) and builds the slice with the proven length.
//
// WHAT THE PROOF IS. N is admitted only when the function provably accesses
// `p[0..N)` and nothing else, on EVERY path:
//   * the body is ONE basic block with no region-carrying operation in it, so
//     there is no path on which an access is skipped (an early `return` is the
//     killer -- see `guarded` in c-abi-exports-slice-bound-refused.c);
//   * no call of any kind, because a callee may `exit()` before the later
//     accesses run, and then the C caller never had to own those bytes;
//   * every operation in the body is on a WHITELIST of pure value operations,
//     never a blacklist, for FR-182's reason;
//   * every use of the parameter is a deref, every use of that deref is a
//     `subscript` at a CONSTANT non-negative index, and every use of a
//     subscript is a `load` -- so the accesses are reads, they are enumerable,
//     and the pointer cannot escape;
//   * at least one access exists, because `from_raw_parts(p, 0)` still
//     requires `p` non-null and a C caller may legally pass NULL for a pointer
//     nothing reads.
// N is then max(index) + 1. That is sound because C itself requires it: a C
// program in which `p[N-1]` is read on every path must already give `p` at
// least N elements, or it has UB before this transpiler is involved.
//
// The element type is fixed at ONE BYTE, and this is deliberate. FR-182
// measured that the emitted Rust primitive is NOT always the same width as the
// C type it came from -- `long double` maps to `f64`, a 128-bit C type
// silently narrowed to 64 -- and nothing in a bare slice type carries the
// importer's faithfulness verdict the way `emitrust.abi_faithful` carries it
// for a record. A one-byte element is the only width for which the emitted
// primitive is provably the C one, and it is also the only element type for
// which the importer ever produces a SHARED `&[u8]` in the first place
// (ImportCTypes.cpp: a walked `const unsigned char *`).
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports %s \
// RUN:   -o %t.cabi 2>%t.cabi.err
// RUN: cat %t.cabi/src/lib.rs | FileCheck %s --check-prefix=CABI
//
// Nothing here is refused, so the flagged run is silent.
// RUN: not grep . %t.cabi.err
//
// Exactly five wrappers; `unsafe` and `from_raw_parts` appear exactly once per
// wrapper and NEVER in a translated body -- the whole point of putting the C
// symbol on a generated item is that the translated function keeps its bytes.
// RUN: grep -c export_name %t.cabi/src/lib.rs > %t.cabi.count
// RUN: grep -c unsafe %t.cabi/src/lib.rs >> %t.cabi.count
// RUN: grep -c from_raw_parts %t.cabi/src/lib.rs >> %t.cabi.count
// RUN: FileCheck %s --check-prefix=COUNT --input-file=%t.cabi.count
//
// The translated functions themselves never take the C ABI: their `&[u8]` is
// not C's pointer, and `extern "C"` on it is exactly the FR-138 mismatch this
// feature exists to avoid. Nothing in this file is all-scalar, so `#[no_mangle]`
// must not appear at all.
// RUN: not grep 'extern "C" fn hdr_bitrate' %t.cabi/src/lib.rs
// RUN: not grep 'extern "C" fn byte_pick' %t.cabi/src/lib.rs
// RUN: not grep no_mangle %t.cabi/src/lib.rs
//
// The byte-identity guard: the flag is default OFF so every `--emit=crate`
// golden stays byte-identical, and the wrapper epilogue is a pure module-scope
// APPEND -- deleting it reproduces the flagless crate root byte for byte.
// RUN: emitrust-cc --emit=crate --crate-type=lib %s -o %t.plain 2>%t.plain.err
// RUN: not grep . %t.plain.err
// RUN: not grep unsafe %t.plain/src/lib.rs
// RUN: not grep export_name %t.plain/src/lib.rs
// RUN: not grep 'extern "C"' %t.plain/src/lib.rs
// RUN: sed '/^#\[export_name/,$d' %t.cabi/src/lib.rs > %t.cabi.stripped
// RUN: diff %t.plain/src/lib.rs %t.cabi.stripped

#include <stdint.h>

// The corpus function this increment exists for, verbatim from
// Public-Tests/B01_organic/hdr_bitrate_lib. It reads h[1] and h[2] in a single
// expression, so the bound is 3 -- and the corpus's own runner hands it a
// `[u8; 3]`, which is the bound exactly.
// CABI: pub fn hdr_bitrate(h: &[u8]) -> u32 {
unsigned hdr_bitrate(const uint8_t *h) {
    static const uint8_t halfrate[2][3][15] = {
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

// A TRAILING SCALAR. This is the shape FR-181's register measurement was made
// on: had the slice been exported directly, `k` would have received the
// synthesized length instead of the caller's integer. The wrapper's `*const u8`
// is one register, so `k` keeps its slot.
// CABI: pub fn byte_pick(p: &[u8], k: i32) -> i32 {
int byte_pick(const unsigned char *p, int k) { return p[0] * k + p[1]; }

// A LEADING SCALAR: the reference is not required to be parameter zero.
// CABI: pub fn lead_scalar(k: i32, p: &[u8]) -> i32 {
int lead_scalar(int k, const unsigned char *p) { return k * 100 + p[0] + p[1]; }

// POINTER ARITHMETIC does not defeat the proof and is not special-cased: the
// bound is read off the CONSTANTS the imported body actually indexes with, so
// `p += 2; p[1]` is index 3 and the bound is 4. Deriving anything from the
// DECLARATOR would be wrong here, which is FR-181's language-law point that
// `T p[N]` in a C prototype is just `T *p`.
// CABI: pub fn bump_read(p: &[u8]) -> u32 {
unsigned bump_read(const unsigned char *p) {
  p += 2;
  return p[1];
}

// A void result is a complete C signature too.
// CABI: pub fn tally(p: &[u8], out: i32) -> i32 {
int tally(const unsigned char *p, int out) { return out + p[0] + p[4]; }

// The wrapper epilogue. Each item is named for the bare C symbol with the
// `__emitrust_cabi_` prefix (reserved in C, so it cannot collide with an
// imported name) and binds the bare symbol through `#[export_name]`, which is
// what lets the translated function above keep its own name and every internal
// call site keep its bytes.
// CABI:      #[export_name = "hdr_bitrate"]
// CABI-NEXT: pub unsafe extern "C" fn __emitrust_cabi_hdr_bitrate(h: *const u8) -> u32 {
// CABI-NEXT:     hdr_bitrate(core::slice::from_raw_parts(h, 3))
// CABI-NEXT: }
// CABI:      #[export_name = "byte_pick"]
// CABI-NEXT: pub unsafe extern "C" fn __emitrust_cabi_byte_pick(p: *const u8, k: i32) -> i32 {
// CABI-NEXT:     byte_pick(core::slice::from_raw_parts(p, 2), k)
// CABI-NEXT: }
// CABI:      #[export_name = "lead_scalar"]
// CABI-NEXT: pub unsafe extern "C" fn __emitrust_cabi_lead_scalar(k: i32, p: *const u8) -> i32 {
// CABI-NEXT:     lead_scalar(k, core::slice::from_raw_parts(p, 2))
// CABI-NEXT: }
// CABI:      #[export_name = "bump_read"]
// CABI-NEXT: pub unsafe extern "C" fn __emitrust_cabi_bump_read(p: *const u8) -> u32 {
// CABI-NEXT:     bump_read(core::slice::from_raw_parts(p, 4))
// CABI-NEXT: }
// CABI:      #[export_name = "tally"]
// CABI-NEXT: pub unsafe extern "C" fn __emitrust_cabi_tally(p: *const u8, out: i32) -> i32 {
// CABI-NEXT:     tally(core::slice::from_raw_parts(p, 5), out)
// CABI-NEXT: }

// COUNT: 5
// COUNT: 5
// COUNT: 5

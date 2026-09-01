// REQUIRES: cargo
// FR-182: THE oracle for the `repr(C)` C-ABI export tier. `cargo build`
// succeeding proves exactly NOTHING about a struct crossing an FFI boundary:
// rustc compiles a repr disagreement with no diagnostic at all, and
// `improper_ctypes_definitions` does not fire for a raw pointer to a
// non-`repr(C)` struct. So this test builds the emitted crate as a real
// cdylib, dlopens the produced shared object from a C host that knows nothing
// but the C ABI and the shared record definitions, dlsyms each BARE symbol,
// calls it through a plain C signature, and byte-diffs the result against the
// clang-built native of the same C source.
//
// MEASURED that this bites: deleting the `#[repr(C)]` lines from the emitted
// crate and rebuilding gives `wide_mix=11000.000000` where the native says
// `7011.125000`, and `tflac_validate` writes `fields=64 1000 67 0 1027` where
// the native writes `64 1000 3 4 67` -- exit 0, no diagnostic, no warning,
// nothing but wrong numbers. That is the entire class this feature exists to
// close, and only a byte-diff sees it.
//
// The signatures are chosen so that no ABI mistake can hide:
//   * `vec_add`/`vec_dot` pass and return the single-SSE-register record;
//   * `wide_mix` passes a record whose eightbytes are classified INTEGER then
//     SSE independently, so a reordering repr misassigns the registers;
//   * `big_shift`/`big_sum` use the 24-byte MEMORY-class record -- passed on
//     the STACK and returned through the hidden sret pointer;
//   * `mix_after` sandwiches two record arguments between four scalars, so a
//     record consuming the wrong number of registers shifts everything after
//     it;
//   * `tflac_validate` takes a POINTER to storage the HOST allocated with the
//     C compiler's layout and writes three fields through it, and the driver
//     reads all five back -- a field-offset disagreement is then visible and
//     not merely possible;
//   * `span_width` takes a pointer to a record containing another record, so
//     the transitive `#[repr(C)]` closure is exercised too.
// Every input derives from argc (via the shared driver's `seed`), so constant
// folding cannot pre-compute an answer on either side, and the second RUN pair
// re-seeds through extra argv words.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports \
// RUN:   --crate-name=cabistructs %s -o %t.crate --build
// RUN: clang -std=c11 -DC_ABI_NATIVE_MAIN %s -o %t.native
// RUN: clang -std=c11 %S/Inputs/c-abi-exports-structs-dlopen-host.c \
// RUN:   -o %t.host -ldl
//
// The manifest really asked for a shared object, and the crate root really
// carries the layout promise -- checked before the run so a packaging or
// gating regression is reported as itself rather than as a dlsym failure.
// RUN: cat %t.crate/Cargo.toml | FileCheck %s --check-prefix=TOML
// RUN: cat %t.crate/src/lib.rs | FileCheck %s --check-prefix=LIB
//
// RUN: %t.native > %t.native.out
// RUN: %t.host %t.crate/target/release/libcabistructs%shlibext > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.host %t.crate/target/release/libcabistructs%shlibext a b \
// RUN:   > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include "Inputs/c-abi-exports-structs-dlopen-types.h"

/* Class 0: the record in and the record out. */
vec2 vec_add(vec2 a, vec2 b) {
  vec2 r;
  r.x = a.x + b.x;
  r.y = a.y + b.y;
  return r;
}

float vec_dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }

double wide_mix(struct wide w, int k) {
  return (double)w.a * 1000.0 + w.b + (double)w.c * 0.5 + (double)k;
}

/* Every field of the result comes from a DIFFERENT field of the argument, so
   a misassigned eightbyte permutes the printed line rather than perturbing
   one number. */
struct big big_shift(struct big b, int k) {
  struct big r;
  r.a = b.b + k;
  r.b = b.c + k;
  r.c = b.d + k;
  r.d = b.e + k;
  r.e = b.f + k;
  r.f = b.a + k;
  return r;
}

int big_sum(struct big b) {
  return b.a + b.b * 2 + b.c * 4 + b.d * 8 + b.e * 16 + b.f * 32;
}

int mix_after(int a, vec2 v, int b, double d, vec2 w, int c) {
  return a + (int)v.x * 2 + b * 4 + (int)d * 8 + (int)w.y * 16 + c * 32;
}

/* Class 1: the host owns the storage, this writes into it. */
int tflac_validate(struct tflac *t, int n) {
  if (t->blocksize < 16u)
    return -1;
  t->cur_blocksize = t->blocksize + (unsigned)n;
  t->channel_mode = (unsigned char)(n & 3);
  t->partition_order = (unsigned char)((n + 1) & 7);
  return (int)t->cur_blocksize;
}

/* Reads every field back with a different weight, so any two fields swapping
   places changes the answer. */
unsigned tflac_peek(struct tflac *t) {
  return t->cur_blocksize * 3u + t->channel_mode * 5u +
         t->partition_order * 7u + t->samplerate;
}

float span_width(struct span *s) {
  return (s->hi.x - s->lo.x) + (s->hi.y - s->lo.y);
}

// The native oracle's entry point, compiled only for the native leg. It runs
// the SAME driver body the dlopen host runs, so the two outputs are comparable
// by construction.
#ifdef C_ABI_NATIVE_MAIN
#include <stdio.h>
#include "Inputs/c-abi-exports-structs-dlopen-driver.h"

int main(int argc, char **argv) {
  (void)argv;
  run_driver(argc);
  return 0;
}
#endif

// TOML:      [lib]
// TOML-NEXT: name = "cabistructs"
// TOML-NEXT: path = "src/lib.rs"
// TOML-NEXT: crate-type = ["cdylib"]

// Every record that crosses is `#[repr(C)]` and asserts clang's numbers, the
// by-value entries carry the C ABI directly, and the pointer entries carry it
// through a wrapper.
// LIB: #[repr(C)]
// LIB: pub struct Vec2 {
// LIB: const _: () = assert!(core::mem::size_of::<Vec2>() == 8);
// LIB: #[repr(C)]
// LIB: pub struct Tflac {
// LIB: const _: () = assert!(core::mem::offset_of!(Tflac, cur_blocksize) == 12);
// LIB: #[repr(C)]
// LIB: pub struct Wide {
// LIB: const _: () = assert!(core::mem::offset_of!(Wide, b) == 8);
// LIB: #[repr(C)]
// LIB: pub struct Big {
// LIB: const _: () = assert!(core::mem::size_of::<Big>() == 24);
// LIB: #[repr(C)]
// LIB: pub struct Span {
// LIB: #[no_mangle]
// LIB: pub extern "C" fn vec_add(
// LIB: #[no_mangle]
// LIB: pub extern "C" fn vec_dot(
// LIB: #[no_mangle]
// LIB: pub extern "C" fn wide_mix(
// LIB: #[no_mangle]
// LIB: pub extern "C" fn big_shift(
// LIB: #[no_mangle]
// LIB: pub extern "C" fn big_sum(
// LIB: #[no_mangle]
// LIB: pub extern "C" fn mix_after(
// LIB: #[export_name = "tflac_validate"]
// LIB: #[export_name = "tflac_peek"]
// LIB: #[export_name = "span_width"]

// REQUIRES: cargo
// FR-75 differential end-to-end test: a requirement-trait pointer
// parameter is a REGION (slice), and a consumer really can implement a
// helper that writes buf[1..] THROUGH it — the shape the pre-FR-75
// single-element ABI (`fn helper(v0: &mut u8, ...)` fed `&mut b[0]`)
// could not express at all.
//
// The C below is a library (no `main`) that calls a helper it never
// defines, once over the whole array and once at cursor 1. The emitted
// trait item must be slice-typed and both call sites must pass reslices
// at their cursors. A separate consumer crate implements `Externals` with
// a helper that rewrites EVERY byte of the region; the identical program
// is provided in C under `#ifdef LIB_CRATE_MAIN`, where the C definition
// of the helper lives, so the native oracle links while the transpiled
// project genuinely cannot see it. The seed is derived from argc (run at
// two argc values) so constant folding cannot hide a miscompile.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate
// RUN: FileCheck --check-prefix=TRAIT %s < %t.crate/src/lib.rs
//
// TRAIT:      pub trait Externals {
// TRAIT-NEXT:     fn helper(v0: &mut [u8], v1: u32);
// TRAIT-NEXT: }
// TRAIT:      pub fn use_it<E: Externals>(
// TRAIT:      let v[[R0:[0-9]+]]: &mut [u8] = &mut b[0i64 as usize..];
// TRAIT-NEXT: E::helper(v[[R0]], 4u32);
// TRAIT:      let v[[R1:[0-9]+]]: &mut [u8] = &mut b[1i64 as usize..];
// TRAIT-NEXT: E::helper(v[[R1]], 3u32);
//
// RUN: rustc --edition=2021 --crate-type=rlib \
// RUN:   --crate-name=lib_crate_externals_slice %t.crate/src/lib.rs -o %t.rlib
// RUN: rustc --edition=2021 --extern lib_crate_externals_slice=%t.rlib \
// RUN:   %S/Inputs/lib-crate-externals-slice-consumer.rs -o %t.consumer
// RUN: %t.consumer > %t.rust.out
// RUN: %t.consumer seed extra > %t.rust3.out
//
// RUN: clang -std=c11 -DLIB_CRATE_MAIN %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.native seed extra > %t.native3.out
// RUN: diff %t.native.out %t.rust.out
// RUN: diff %t.native3.out %t.rust3.out

// Declared, never defined here: this is the crate's requirement.
void helper(unsigned char *buf, unsigned int len);

int use_it(int seed) {
  unsigned char b[4] = {1, 2, 3, 4};
  b[0] = (unsigned char)seed;
  helper(b, 4u);
  helper(b + 1, 3u);
  return b[0] + b[1] * 10 + b[2] * 100 + b[3] * 1000;
}

// The native oracle: the same driver the Rust consumer runs, plus the C
// definition of the helper — a MULTI-BYTE writer over the whole region.
#ifdef LIB_CRATE_MAIN
int printf(const char *, ...);

void helper(unsigned char *buf, unsigned int len) {
  unsigned int i;
  for (i = 0; i < len; i++)
    buf[i] = (unsigned char)(buf[i] * 2u + i);
}

int main(int argc, char **argv) {
  (void)argv;
  printf("use_it=%d\n", use_it(argc));
  return 0;
}
#endif

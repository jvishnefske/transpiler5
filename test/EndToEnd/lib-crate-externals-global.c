// REQUIRES: cargo
// FR-70 differential end-to-end test: a LIBRARY crate whose external GLOBAL
// -- storage it reads and writes but never defines -- is supplied by its
// consumer through a getter/setter pair on the emitted `Externals` trait.
//
// The C below is a library (no `main`) built around `extern int g_config`,
// which no translation unit defines. Before FR-70 that was a whole-program
// error and the project produced NO crate at all (the single largest
// crate-killer in the Track 5 corpus). Now the importer records the global
// as a requirement, `emitrust-lower-external-requirements` rewrites every
// whole-value load to `E::g_config()` and every store to
// `E::set_g_config(v)`, and the crate compiles -- with no dyn, no unsafe,
// and no fabricated storage.
//
// The test then proves the requirement is SATISFIABLE and the semantics
// EXACT, which a compile alone cannot: a separate consumer crate backs the
// pair with real storage seeded with the same initializer the native leg's
// `int g_config = 7;` carries, and the identical driver is provided in C
// under `#ifdef LIB_CRATE_MAIN` -- which is also where the definitions of
// `g_config` and `host_scale` live, so the native oracle links while the
// transpiled project genuinely cannot see them. Both drivers seed from argc
// so constant folding cannot hide a miscompile, and the read-after-write in
// `bump` makes the byte-diff sensitive to stores that fail to reach
// subsequent loads.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate
// RUN: FileCheck --check-prefix=TRAIT %s < %t.crate/src/lib.rs
//
// The function requirement and the global's getter/setter share ONE trait
// (functions first, then each global's getter before its setter), and only
// the functions that touch either grew a type parameter.
// TRAIT:      pub trait Externals {
// TRAIT-NEXT:     fn host_scale(v0: i32) -> i32;
// TRAIT-NEXT:     fn g_config() -> i32;
// TRAIT-NEXT:     fn set_g_config(v0: i32);
// TRAIT-NEXT: }
// TRAIT:      pub fn bump<E: Externals>(
// TRAIT:      pub fn peek<E: Externals>(
// TRAIT:      pub fn scaled<E: Externals>(
// TRAIT:      pub fn plain_sum(a: i32, b: i32) -> i32 {
//
// RUN: rustc --edition=2021 --crate-type=rlib \
// RUN:   --crate-name=lib_crate_externals_global %t.crate/src/lib.rs -o %t.rlib
// RUN: rustc --edition=2021 --extern lib_crate_externals_global=%t.rlib \
// RUN:   %S/Inputs/lib-crate-externals-global-consumer.rs -o %t.consumer
// RUN: %t.consumer > %t.rust.out
//
// RUN: clang -std=c11 -DLIB_CRATE_MAIN %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: diff %t.native.out %t.rust.out

// Declared, never defined here: the crate's requirements on its environment.
extern int g_config;
int host_scale(int v);

// Read-after-write across the trait boundary: the store must be visible to
// every later load, exactly as the C global's would be.
int bump(int d) {
  int old = g_config;
  g_config = old + d;
  return old;
}

// Read-only reach: generic all the same, through the getter.
int peek(void) { return g_config; }

// Mixed: a function requirement and the global in one expression.
int scaled(int v) { return host_scale(v) + g_config; }

// Reaches nothing: must keep exactly the signature it always had.
int plain_sum(int a, int b) { return a + b; }

// The native oracle: the same driver the Rust consumer runs, plus the C
// definitions of the requirements. Compiled only for the native leg, so the
// transpiled project really does see them as undefined.
#ifdef LIB_CRATE_MAIN
int printf(const char *, ...);

int g_config = 7;
int host_scale(int v) { return v * 3; }

int main(int argc, char **argv) {
  (void)argv;
  int seed = argc * 4; /* 4 when run plain, but opaque to the compiler */
  printf("peek=%d\n", peek());
  printf("bump=%d\n", bump(seed + 1));
  printf("peek=%d\n", peek());
  printf("scaled=%d\n", scaled(seed));
  printf("plain=%d\n", plain_sum(seed, 5));
  return 0;
}
#endif

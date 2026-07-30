// REQUIRES: cargo
// FR-52 differential end-to-end test: a LIBRARY crate whose external
// requirements are supplied by its consumer through the emitted trait.
//
// The C below is a library (no `main`) that calls two functions it never
// defines. Before FR-52 that was a whole-program error and this project
// produced NO crate at all. Now the importer records the two symbols as
// requirements, `emitrust-lower-external-requirements` emits
// `pub trait Externals` and makes the transitive closure of their callers
// generic over it, and the crate compiles.
//
// The test then proves the requirement is really SATISFIABLE, which a
// compile alone does not show: a separate consumer crate implements
// `Externals` and calls the generic functions, and the identical program is
// provided in C by `#ifdef LIB_CRATE_MAIN` -- which is also where the C
// definitions of the two "external" functions live, so the native oracle
// links while the transpiled project genuinely cannot see them.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate
// RUN: FileCheck --check-prefix=TRAIT %s < %t.crate/src/lib.rs
//
// The trait carries exactly the two undefined symbols, and only the
// functions that (transitively) need them grew a type parameter.
// TRAIT:      pub trait Externals {
// TRAIT-NEXT:     fn host_scale(v0: i32) -> i32;
// TRAIT-NEXT:     fn host_floor(v0: i32) -> i32;
// TRAIT-NEXT: }
// TRAIT:      pub fn scaled<E: Externals>(
// TRAIT:      pub fn banded<E: Externals>(
// TRAIT:      pub fn plain_sum(v0: i32, v1: i32) -> i32 {
//
// RUN: rustc --edition=2021 --crate-type=rlib \
// RUN:   --crate-name=lib_crate_externals_trait %t.crate/src/lib.rs -o %t.rlib
// RUN: rustc --edition=2021 --extern lib_crate_externals_trait=%t.rlib \
// RUN:   %S/Inputs/lib-crate-externals-consumer.rs -o %t.consumer
// RUN: %t.consumer > %t.rust.out
//
// RUN: clang -std=c11 -DLIB_CRATE_MAIN %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: diff %t.native.out %t.rust.out

// Declared, never defined here: these are the crate's requirements.
int host_scale(int v);
int host_floor(int v);

int scaled(int v) { return host_scale(v) + 1; }

// Reaches a requirement only through `scaled`, so it is generic for the
// transitive reason rather than the direct one.
int banded(int v) {
  int s = scaled(v);
  if (s < host_floor(s))
    return host_floor(s);
  return s;
}

// Reaches nothing: must keep exactly the signature it always had.
int plain_sum(int a, int b) { return a + b; }

// The native oracle: the same driver the Rust consumer runs, plus the C
// definitions of the two requirements. Compiled only for the native leg, so
// the transpiled project really does see them as undefined.
#ifdef LIB_CRATE_MAIN
int printf(const char *, ...);

int host_scale(int v) { return v * 3; }
int host_floor(int v) { return v < 10 ? 10 : v; }

int main(void) {
  printf("scaled=%d\n", scaled(4));
  printf("banded=%d\n", banded(1));
  printf("banded=%d\n", banded(7));
  printf("plain=%d\n", plain_sum(2, 5));
  return 0;
}
#endif

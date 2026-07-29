// REQUIRES: cargo
// FR-51 differential end-to-end test for a LIBRARY crate.
//
// The C below is a library: an accumulator API with no `main`, so
// emitrust-cc emits a cargo library crate (src/lib.rs, a [lib] manifest
// section, no entry-point wrapper). A library has nothing to run, which is
// precisely why the RealWorld harness scores such a project LIB_BUILT rather
// than TRANSPILED -- there is no executable to diff against a native build.
//
// This test recovers that lost evidence by SUPPLYING the missing entry point
// on both sides. The emitted crate is compiled as an rlib and linked into a
// separate consumer crate (Inputs/lib-crate-consumer.rs), and the identical
// driver is provided in C by `#ifdef LIB_CRATE_MAIN`, compiled natively. The
// two must print the same bytes.
//
// The consumer being a SEPARATE crate is the load-bearing part: it can only
// name items the emitter marked `pub`, so this test fails if the library
// exports nothing -- and `scale_hidden` below fails to compile into it if the
// emitter wrongly exports internal-linkage items.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate
// RUN: rustc --edition=2021 --crate-type=rlib \
// RUN:   --crate-name=lib_crate_external_caller %t.crate/src/lib.rs \
// RUN:   -o %t.rlib
// RUN: rustc --edition=2021 --extern lib_crate_external_caller=%t.rlib \
// RUN:   %S/Inputs/lib-crate-consumer.rs -o %t.consumer
// RUN: %t.consumer > %t.rust.out
//
// RUN: clang -std=c11 -DLIB_CRATE_MAIN %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: diff %t.native.out %t.rust.out
//
// The private item really is private: naming it from the consumer crate is
// an E0603, which is the direct evidence that a C `static` did NOT become
// part of the library's public API.
// RUN: echo 'fn main() { lib_crate_external_caller::tu0_scale_hidden(1); }' > %t.private.rs
// RUN: not rustc --edition=2021 --crate-name=privcheck \
// RUN:   --extern lib_crate_external_caller=%t.rlib \
// RUN:   %t.private.rs -o %t.private 2>&1 | FileCheck %s --check-prefix=PRIVATE
// PRIVATE: E0603

struct Acc {
  int count;
  int total;
};

void acc_reset(struct Acc *a) {
  a->count = 0;
  a->total = 0;
}

void acc_push(struct Acc *a, int value) {
  a->count = a->count + 1;
  a->total = a->total + value;
}

int acc_count(const struct Acc *a) { return a->count; }

int acc_total(const struct Acc *a) { return a->total; }

// Internal linkage: must stay private in the emitted crate.
static int scale_hidden(int v, int by) { return v * by; }

int scale_total(const struct Acc *a, int by) {
  return scale_hidden(a->total, by);
}

int clamped(int v) {
  if (v < 0)
    return 0;
  if (v > 10)
    return 10;
  return v;
}

// The native oracle's driver, compiled only for the native leg. It is the
// same sequence of calls the Rust consumer makes, so the emitted library and
// the native library are compared on observable behavior, not just on
// whether they build.
#ifdef LIB_CRATE_MAIN
int printf(const char *, ...);

int main(void) {
  struct Acc acc;
  acc_reset(&acc);
  int values[4] = {7, -3, 20, 5};
  for (int i = 0; i < 4; i++)
    acc_push(&acc, values[i]);
  printf("count=%d\n", acc_count(&acc));
  printf("total=%d\n", acc_total(&acc));
  printf("scaled=%d\n", scale_total(&acc, 3));
  printf("clamped=%d\n", clamped(-9));
  printf("clamped=%d\n", clamped(11));
  return 0;
}
#endif

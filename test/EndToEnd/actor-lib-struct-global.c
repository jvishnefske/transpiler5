// REQUIRES: cargo
// FR-84 differential end-to-end test: an in-TU-defined STRUCT global with
// an aggregate initializer exported as an owner handle (library TU). The
// actor plan stages the C initializer as a const emitrust.variable inside
// the owner impl's synthesized new(), and `emitrust.impl` is a SymbolTable
// of its own — the old nearest-table struct_def lookup in the aggregate
// verifier never saw the module-level def and killed the whole crate (the
// lwIP ip4_addr/ip6_addr loss). This pins the fixed pipeline end to end:
// the crate builds as an rlib, a hand-written consumer
// (Inputs/actor-lib-struct-global-consumer.rs) constructs the owner
// through new() and exercises every arm, and its stdout must byte-diff
// clean against the clang-built native running the identical driver
// (`#ifdef LIB_CRATE_MAIN`). Inputs are seeded from argc / args().count()
// so constant folding cannot hide a miscompiled initializer or arm.
// Adversarial field shapes on purpose: negative int, width-extreme
// unsigned, exact-representable negative double, and a nested array with
// an implicit zero tail — new() must reproduce them all.
// RUN: emitrust-cc --emit=crate %s -o %t.crate
// RUN: rustc --edition=2021 --crate-type=rlib \
// RUN:   --crate-name=actor_lib_struct_global %t.crate/src/lib.rs \
// RUN:   --out-dir %t.crate
// RUN: rustc --edition=2021 \
// RUN:   --extern actor_lib_struct_global=%t.crate/libactor_lib_struct_global.rlib \
// RUN:   %S/Inputs/actor-lib-struct-global-consumer.rs -o %t.consumer
// RUN: %t.consumer > %t.rust.out
//
// RUN: clang -std=c11 -DLIB_CRATE_MAIN %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: diff %t.native.out %t.rust.out

// The exported actor: external-linkage arms over one struct global whose
// aggregate initializer is the FR-84 trigger shape.
struct Cal {
  int base;
  unsigned int mask;
  double scale;
  int taps[3];
};

struct Cal cal = {-7, 4000000000u, -1.25, {2, -3}};

int shift_base(int by) {
  cal.base = cal.base + by;
  return cal.base;
}

unsigned int fold_mask(unsigned int m) {
  cal.mask = cal.mask ^ m;
  return cal.mask;
}

double scaled(int v) { return cal.scale * v; }

int tap_sum(void) { return cal.taps[0] + cal.taps[1] + cal.taps[2]; }

// The native oracle's driver, compiled only for the native leg: the same
// argc-seeded call sequence the Rust consumer makes, so the two libraries
// are compared on observable behavior.
#ifdef LIB_CRATE_MAIN
int printf(const char *, ...);

int main(int argc, char **argv) {
  (void)argv;
  int seed = argc; /* 1 when run bare; the consumer mirrors it */
  printf("base=%d\n", shift_base(seed * 5));
  printf("base=%d\n", shift_base(-2 * seed));
  printf("mask=%lu\n", (unsigned long)fold_mask(3855u * (unsigned)seed));
  printf("scaled=%f\n", scaled(3 * seed));
  printf("taps=%d\n", tap_sum());
  return 0;
}
#endif

// REQUIRES: cargo
// FR-85 differential end-to-end test: a LIBRARY crate whose CONST
// BYTE-REGION external global -- the lwIP `extern const struct eth_addr
// ethbroadcast` shape, a u8-only record that imports as a plain
// `[u8; 6]` byte region with no struct definition -- is supplied by its
// consumer through a GETTER-ONLY item on the emitted `Externals` trait.
//
// The C below is a library (no `main`) built around a byte-region extern
// no translation unit defines. FR-79 admitted const STRUCT externs via a
// by-value getter; byte-region records never reach that arm (no struct_def
// exists for them), so etharp.c-shaped code kept the whole-crate rejection.
// FR-85 admits the const byte region the same by-value way, and the
// crucial semantic fact under test is the importer's staged-copy image:
// `&ethbroadcast` at an argument position is a whole-region copy plus a
// shared byte slice OF THE COPY, so the by-value `fn ethbroadcast() ->
// [u8; 6]` getter reproduces the C bytes exactly (pointer identity was
// never preserved for byte regions, so none is lost).
//
// The test then proves the requirement is SATISFIABLE and the semantics
// EXACT, which a compile alone cannot: a separate consumer crate backs the
// getter with the same six bytes the native leg's definition carries, and
// backs the body-less `ethernet_output` requirement with the same printf
// the native leg's definition performs -- both live under
// `#ifdef LIB_CRATE_MAIN`, so the native oracle links while the transpiled
// project genuinely cannot see them. Both drivers seed from argc so
// constant folding cannot hide a miscompile.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate
// RUN: FileCheck --check-prefix=TRAIT %s < %t.crate/src/lib.rs
//
// The getter is the ONLY item for the global -- no setter exists for a
// const requirement -- and its spelling is the snake_case of the FR-53
// upper-snake symbol (`ETHBROADCAST` -> `ethbroadcast`). The body-less
// callee's item takes the byte-region parameter as the shared byte slice.
// TRAIT:      pub trait Externals {
// TRAIT-DAG:      fn ethernet_output(v0: &[u8]);
// TRAIT-DAG:      fn ethbroadcast() -> [u8; 6];
// TRAIT:      }
// TRAIT-NOT:  set_ethbroadcast
// TRAIT:      pub fn eth_sum<E: Externals>(
// TRAIT:      pub fn plain_sum(a: i32, b: i32) -> i32 {
//
// RUN: rustc --edition=2021 --crate-type=rlib \
// RUN:   --crate-name=lib_crate_externals_const_byteregion %t.crate/src/lib.rs \
// RUN:   -o %t.rlib
// RUN: rustc --edition=2021 \
// RUN:   --extern lib_crate_externals_const_byteregion=%t.rlib \
// RUN:   %S/Inputs/lib-crate-externals-const-byteregion-consumer.rs -o %t.consumer
// RUN: %t.consumer > %t.rust.out
//
// RUN: clang -std=c11 -DLIB_CRATE_MAIN %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: diff %t.native.out %t.rust.out

typedef unsigned char u8_t;

// A u8-only record: padding-free by construction, imported as a byte
// region ([u8; 6]) with no struct definition ever emitted.
struct eth_addr {
  u8_t addr[6];
} __attribute__((packed));

// Declared const, never defined here: the crate's read-only requirement on
// its environment.
extern const struct eth_addr ethbroadcast;

// Body-less: the function requirement the consumer must also satisfy (the
// lwIP ethernet_output shape).
void ethernet_output(const struct eth_addr *dst);

// Argument-pass of the region's address to the requirement callee, plus
// element reads -- the etharp shapes. Seeded so nothing folds.
int eth_sum(int seed) {
  ethernet_output(&ethbroadcast);
  return seed + (int)ethbroadcast.addr[0] + (int)ethbroadcast.addr[5];
}

// Reaches nothing: must keep exactly the signature it always had.
int plain_sum(int a, int b) { return a + b; }

// The native oracle: the same driver the Rust consumer runs, plus the C
// definitions of both requirements. Compiled only for the native leg, so
// the transpiled project really does see them as undefined.
#ifdef LIB_CRATE_MAIN
int printf(const char *, ...);

const struct eth_addr ethbroadcast = {{0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa}};

void ethernet_output(const struct eth_addr *dst) {
  printf("out %u %u\n", (unsigned)dst->addr[0], (unsigned)dst->addr[3]);
}

int main(int argc, char **argv) {
  (void)argv;
  printf("%d\n", eth_sum(argc));
  printf("plain=%d\n", plain_sum(argc, 5));
  return 0;
}
#endif

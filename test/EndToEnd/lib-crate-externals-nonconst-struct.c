// REQUIRES: cargo
// FR-81 differential end-to-end test: a LIBRARY crate whose NON-const
// STRUCT external global -- the lwIP `extern struct ip_globals ip_data`
// shape -- is supplied by its consumer through a whole-value GETTER/SETTER
// pair on the emitted `Externals` trait.
//
// The C below is a library (no `main`) built around `extern struct
// ip_globals ip_data`, which no translation unit defines. FR-70 admitted
// scalar extern globals as getter/setter pairs; FR-79 admitted const
// structs getter-only; FR-81 closes the matrix with the non-const struct:
// sequentially -- and the dialect's mutable-global model is already exact
// only for the single-threaded programs the importer accepts -- a staged
// whole-value get/modify/set through a Copy struct is EXACT. The functions
// below cover every access shape the importer lowers, including the two
// SEQUENCING hazards the staging discipline must survive: `swapish` reads
// the global twice while a set is pending (each RHS read is a fresh getter
// call), and `hazard`'s RHS call mutates the same global, forcing the
// staged copy to be REFRESHED before the member write lands.
//
// The test then proves the requirement is SATISFIABLE and the semantics
// EXACT, which a compile alone cannot: a separate consumer crate backs the
// pair with real storage -- a thread-local Cell<IpGlobals> seeded with the
// same `{7, 40u}` the native leg's definition carries, under
// `#ifdef LIB_CRATE_MAIN` so the native oracle links while the transpiled
// project genuinely cannot see it. Both drivers seed from argc so constant
// folding cannot hide a miscompile, and every write is re-read across a
// call boundary, so a setter elided as a "dead store" (external storage is
// observable; eliding it would be a miscompile) breaks the byte-diff.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate
// RUN: FileCheck --check-prefix=TRAIT %s < %t.crate/src/lib.rs
//
// The getter and setter pass the struct BY VALUE both ways; the item
// spelling is the snake_case of the FR-53 upper-snake symbol (`IP_DATA` ->
// `ip_data`/`set_ip_data`). Every function touching the global grew the
// type parameter; the one touching nothing kept its signature.
// TRAIT:      pub trait Externals {
// TRAIT-NEXT:     fn ip_data() -> IpGlobals;
// TRAIT-NEXT:     fn set_ip_data(v0: IpGlobals);
// TRAIT-NEXT: }
// TRAIT:      pub struct IpGlobals {
// TRAIT-NEXT:     pub ttl: i32,
// TRAIT-NEXT:     pub addr: u32,
// TRAIT-NEXT: }
// TRAIT:      pub fn get_ttl<E: Externals>(
// TRAIT:      pub fn set_ttl<E: Externals>(
// TRAIT:      pub fn plain_sum(a: i32, b: i32) -> i32 {
//
// RUN: rustc --edition=2021 --crate-type=rlib \
// RUN:   --crate-name=lib_crate_externals_nonconst_struct %t.crate/src/lib.rs \
// RUN:   -o %t.rlib
// RUN: rustc --edition=2021 \
// RUN:   --extern lib_crate_externals_nonconst_struct=%t.rlib \
// RUN:   %S/Inputs/lib-crate-externals-nonconst-struct-consumer.rs \
// RUN:   -o %t.consumer
// RUN: %t.consumer > %t.rust.out
//
// RUN: clang -std=c11 -DLIB_CRATE_MAIN %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: diff %t.native.out %t.rust.out

struct ip_globals {
  int ttl;
  unsigned int addr;
};

// Declared, never defined here: the crate's mutable requirement on its
// environment.
extern struct ip_globals ip_data;

// Field reads: whole-value getter, member on the Copy temporary.
int get_ttl(void) { return ip_data.ttl; }
unsigned int get_addr(void) { return ip_data.addr; }

// Field write: staged get/modify/set -- the store must be visible to every
// later load, exactly as the C global's would be.
void set_ttl(int t) { ip_data.ttl = t; }

// Compound assign through a field: the same read-modify-write.
void bump(void) { ip_data.ttl += 2; }

// Multi-access statement: the global is read twice while a set is pending;
// each RHS read is a FRESH getter call, so the result is C-sequenced.
void swapish(void) { ip_data.ttl = (int)ip_data.addr + ip_data.ttl; }

// RHS call mutating the same global: the staged copy is refreshed after the
// call, so mutate_addr's write to `addr` survives hazard's write to `ttl`.
int mutate_addr(void) {
  ip_data.addr = 99u;
  return 5;
}
void hazard(void) { ip_data.ttl = mutate_addr(); }

// Whole-struct read and write.
struct ip_globals snapshot(void) { return ip_data; }
void restore(struct ip_globals s) { ip_data = s; }

// Reaches nothing: must keep exactly the signature it always had.
int plain_sum(int a, int b) { return a + b; }

// The native oracle: the same driver the Rust consumer runs, plus the C
// definition of the requirement. Compiled only for the native leg, so the
// transpiled project really does see it as undefined.
#ifdef LIB_CRATE_MAIN
int printf(const char *, ...);

struct ip_globals ip_data = {7, 40u};

int main(int argc, char **argv) {
  (void)argv;
  int seed = argc * 4; /* 4 when run plain, but opaque to the compiler */
  printf("get=%d addr=%u\n", get_ttl(), get_addr());
  set_ttl(seed + 1);
  printf("after set: %d %u\n", get_ttl(), get_addr());
  bump();
  printf("after bump: %d %u\n", get_ttl(), get_addr());
  swapish();
  printf("after swapish: %d %u\n", get_ttl(), get_addr());
  hazard();
  printf("after hazard: %d %u\n", get_ttl(), get_addr());
  struct ip_globals s = snapshot();
  s.ttl += seed;
  restore(s);
  printf("after restore: %d %u\n", get_ttl(), get_addr());
  printf("plain=%d\n", plain_sum(seed, 5));
  return 0;
}
#endif

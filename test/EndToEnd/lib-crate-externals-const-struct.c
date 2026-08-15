// REQUIRES: cargo
// FR-79 differential end-to-end test: a LIBRARY crate whose CONST STRUCT
// external global -- the lwIP `extern const ip_addr_t ip_addr_any` shape --
// is supplied by its consumer through a GETTER-ONLY item on the emitted
// `Externals` trait.
//
// The C below is a library (no `main`) built around
// `extern const struct ip_addr ip_addr_any`, which no translation unit
// defines. FR-70 admitted scalar extern globals as getter/setter pairs and
// kept aggregates rejected; FR-79 admits exactly the READ-ONLY aggregate:
// the struct imports, so it is Copy and a by-value `fn ip_addr_any() ->
// IpAddr` is a faithful read, and const-ness means no writer ever needs a
// setter. The three functions cover the three access shapes the importer
// lowers -- direct field projection, whole-struct read, and read into a
// local then field -- all of which reach the trait through the ONE
// whole-value load the FR-70 rewrite already handles.
//
// The test then proves the requirement is SATISFIABLE and the semantics
// EXACT, which a compile alone cannot: a separate consumer crate backs the
// getter with the same `{0u, 4}` the native leg's definition carries --
// which lives under `#ifdef LIB_CRATE_MAIN`, so the native oracle links
// while the transpiled project genuinely cannot see it. Both drivers seed
// from argc so constant folding cannot hide a miscompile.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate
// RUN: FileCheck --check-prefix=TRAIT %s < %t.crate/src/lib.rs
//
// The getter is the ONLY item -- no setter exists for a const requirement --
// and its spelling is the snake_case of the FR-53 upper-snake symbol
// (`IP_ADDR_ANY` -> `ip_addr_any`). Every function touching the global grew
// the type parameter; the one touching nothing kept its signature.
// TRAIT:      pub trait Externals {
// TRAIT-NEXT:     fn ip_addr_any() -> IpAddr;
// TRAIT-NEXT: }
// TRAIT-NOT:  set_ip_addr_any
// TRAIT:      pub struct IpAddr {
// TRAIT-NEXT:     pub addr: u32,
// TRAIT-NEXT:     pub kind: i32,
// TRAIT-NEXT: }
// TRAIT:      pub fn is_any<E: Externals>(
// TRAIT:      pub fn copy_any<E: Externals>(
// TRAIT:      pub fn kind_plus<E: Externals>(
// TRAIT:      pub fn plain_sum(a: i32, b: i32) -> i32 {
//
// RUN: rustc --edition=2021 --crate-type=rlib \
// RUN:   --crate-name=lib_crate_externals_const_struct %t.crate/src/lib.rs \
// RUN:   -o %t.rlib
// RUN: rustc --edition=2021 \
// RUN:   --extern lib_crate_externals_const_struct=%t.rlib \
// RUN:   %S/Inputs/lib-crate-externals-const-struct-consumer.rs -o %t.consumer
// RUN: %t.consumer > %t.rust.out
//
// RUN: clang -std=c11 -DLIB_CRATE_MAIN %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: diff %t.native.out %t.rust.out

struct ip_addr {
  unsigned int addr;
  int kind;
};

// Declared const, never defined here: the crate's read-only requirement on
// its environment.
extern const struct ip_addr ip_addr_any;

// Field projection through the extern const struct: lowers as a whole-value
// read into a temporary, then a member read of that Copy temporary.
int is_any(unsigned int a) { return a == ip_addr_any.addr; }

// Whole-struct read: the getter's by-value return flows straight through.
struct ip_addr copy_any(void) { return ip_addr_any; }

// Read into a named local, then a field of it.
int kind_plus(int d) {
  struct ip_addr t = ip_addr_any;
  return t.kind + d;
}

// Reaches nothing: must keep exactly the signature it always had.
int plain_sum(int a, int b) { return a + b; }

// The native oracle: the same driver the Rust consumer runs, plus the C
// definition of the requirement. Compiled only for the native leg, so the
// transpiled project really does see it as undefined.
#ifdef LIB_CRATE_MAIN
int printf(const char *, ...);

const struct ip_addr ip_addr_any = {0u, 4};

int main(int argc, char **argv) {
  (void)argv;
  printf("is_any=%d\n", is_any((unsigned int)(argc - 1)));
  struct ip_addr c = copy_any();
  printf("copy=%u,%d\n", c.addr + (unsigned int)argc, c.kind);
  printf("kind=%d\n", kind_plus(argc));
  printf("plain=%d\n", plain_sum(argc, 5));
  return 0;
}
#endif

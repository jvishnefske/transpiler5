// FR-139: `--c-abi-exports` is the opt-in that turns an emitted LIBRARY crate
// into something a C host can actually reach: `crate-type = ["cdylib"]` in the
// manifest so cargo produces a real shared object, and
// `#[no_mangle] pub extern "C" fn` on the exports so there is a BARE symbol to
// dlsym. Without it the crate is an rlib of Rust-ABI, Rust-mangled `pub fn`,
// which no dlopen/dlsym harness can call at all.
//
// This file pins three invariants at once.
//
// 1. WITH the flag, an ALL-SCALAR exported function gets the C-ABI shape.
//    All-scalar means every input and every result of the function type is a
//    builtin integer or floating-point type; a void result and an empty
//    parameter list both qualify.
//
// 2. WITHOUT the flag nothing moves. The flag defaults OFF precisely so that
//    every `--emit=crate` golden stays byte-identical, so the no-flag run is
//    checked here explicitly -- no `#[no_mangle]`, no `extern "C"`, no
//    `crate-type`, not one byte on stderr -- and the flagged crate root is
//    diffed back against it with only those two additions undone.
//
// 3. A NON-scalar exported signature is NOT given `extern "C"`. This is the
//    whole safety argument, not a limitation to be relaxed later: a `&[u8]`
//    parameter is a two-register fat pointer, rustc compiles the mismatch with
//    only a non-FFI-safe WARNING, and every later argument shifts (measured in
//    the FR-138 spike on crc16: native 27235, cdylib 0). Silent wrong code
//    across the boundary is the class this repo forbids, so such a function
//    keeps its plain `pub fn` -- and says so with a LOCATED warning, because a
//    dlsym failing at evaluation time with no explanation is the other way to
//    lose. A warning and not an error: a crate holds many functions and only
//    one is usually the dlsym target.
//
// Internal-linkage functions are not exported in the first place, so they get
// neither the attribute nor the warning.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports %s \
// RUN:   -o %t.cabi 2>%t.cabi.err
// RUN: cat %t.cabi/Cargo.toml | FileCheck %s --check-prefix=CABITOML
// RUN: cat %t.cabi/src/lib.rs | FileCheck %s --check-prefix=CABI
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.cabi.err
//
// The C-ABI shape reaches exactly the all-scalar exports: never the
// internal-linkage function, never the slice-taking one, and exactly three
// times in all.
// RUN: not grep 'extern "C" fn tu0_helper' %t.cabi/src/lib.rs
// RUN: not grep 'extern "C" fn sum_bytes' %t.cabi/src/lib.rs
// RUN: not grep 'pub fn tu0_helper' %t.cabi/src/lib.rs
// RUN: grep -c no_mangle %t.cabi/src/lib.rs > %t.cabi.count
// RUN: FileCheck %s --check-prefix=COUNT --input-file=%t.cabi.count
//
// FR-179, the follow-on that wraps an FR-62 actor-lifted method in a
// module-scope singleton (test/Driver/c-abi-exports-actor.c) must not reach a
// crate that has no lifted state: nothing here is a method, so no singleton
// and no delegating wrapper may appear. This narrows the pin above -- the
// three exports are exactly the three direct ones -- and it is what keeps the
// byte-for-byte diff below a diff of only the attribute and the ABI spelling.
// RUN: not grep __EMITRUST_ACTOR %t.cabi/src/lib.rs
// RUN: not grep thread_local %t.cabi/src/lib.rs
//
// The byte-identity guard: the SAME input without the flag is today's output.
// RUN: emitrust-cc --emit=crate --crate-type=lib %s -o %t.plain 2>%t.plain.err
// RUN: cat %t.plain/Cargo.toml | FileCheck %s --check-prefix=PLAINTOML
// RUN: cat %t.plain/src/lib.rs | FileCheck %s --check-prefix=PLAIN
// RUN: not grep no_mangle %t.plain/src/lib.rs
// RUN: not grep 'extern "C"' %t.plain/src/lib.rs
// RUN: not grep crate-type %t.plain/Cargo.toml
// RUN: not grep . %t.plain.err
//
// ...and the flagged crate root differs from it in NOTHING but those two
// additions: dropping the attribute lines and the `extern "C" ` reproduces the
// default root byte for byte.
// RUN: grep -v no_mangle %t.cabi/src/lib.rs \
// RUN:   | sed 's/^pub extern "C" fn /pub fn /' > %t.cabi.stripped
// RUN: diff %t.plain/src/lib.rs %t.cabi.stripped

// An exported all-scalar function: two integer parameters, an integer result.
int scale(int v, int k) { return v * k; }

// Floating-point scalars qualify too, and so does a mixed-width signature.
double blend(double a, float b) { return a + (double)b; }

// A void result and an empty parameter list both qualify.
void tick(void) {}

// Internal linkage: never exported, so never given a C-ABI symbol either --
// and no warning, because nothing was ever promised for it.
static int helper(int x) { return x + 1; }

// A pointer parameter lowers to a `&[u8]` slice, which is a TWO-register fat
// pointer. This one keeps `pub fn` and is reported, at its own location.
// WARN: c-abi-exports.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'sum_bytes': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int sum_bytes(const unsigned char *p, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i)
    s += p[i];
  return s + helper(0);
}

// The [lib] section gains the cdylib line and nothing else; the deny table is
// untouched by the flag.
// CABITOML:      [lib]
// CABITOML-NEXT: name = "c_abi_exports"
// CABITOML-NEXT: path = "src/lib.rs"
// CABITOML-NEXT: crate-type = ["cdylib"]
// CABITOML:      [lints.rust]
// CABITOML-NEXT: unused_variables = "deny"
// CABITOML:      non_snake_case = "deny"

// CABI:      #[no_mangle]
// CABI-NEXT: pub extern "C" fn scale(v: i32, k: i32) -> i32 {
// CABI:      #[no_mangle]
// CABI-NEXT: pub extern "C" fn blend(a: f64, b: f32) -> f64 {
// CABI:      #[no_mangle]
// CABI-NEXT: pub extern "C" fn tick() {
// CABI-NOT:  #[no_mangle]
// CABI:      fn tu0_helper(x: i32) -> i32 {
// CABI-NOT:  #[no_mangle]
// CABI:      pub fn sum_bytes(p: &[u8], n: i32) -> i32 {

// COUNT: 3

// PLAINTOML:      [lib]
// PLAINTOML-NEXT: name = "c_abi_exports"
// PLAINTOML-NEXT: path = "src/lib.rs"
// PLAINTOML:      [lints.rust]

// PLAIN:      pub fn scale(v: i32, k: i32) -> i32 {
// PLAIN:      pub fn blend(a: f64, b: f32) -> f64 {
// PLAIN:      pub fn tick() {
// PLAIN:      fn tu0_helper(x: i32) -> i32 {
// PLAIN:      pub fn sum_bytes(p: &[u8], n: i32) -> i32 {

// FR-52: which crate shape may express an unresolved external as a
// REQUIREMENT, and which must keep treating it as an error.
//
// The rule is a function of `--crate-type` alone:
//   lib  -> requirement, always;
//   bin  -> error, always (a binary's `fn main` is not generic and has
//           nothing to instantiate it with);
//   auto -> error iff the project defines `main`, because that is exactly
//           when `auto` picks a binary crate.
//
// RUN: split-file %s %t

// A LIBRARY project (no main): `auto` already emits the trait, and the whole
// project translates -- nothing is stubbed and nothing is dropped.
// RUN: emitrust-cc --emit=rust %t/library.c -o - | FileCheck --check-prefix=LIB %s
// LIB:      pub trait Externals {
// LIB-NEXT:     fn host_scale(v0: i32) -> i32;
// LIB-NEXT: }
// LIB:      pub fn scaled<E: Externals>(v: i32) -> i32 {
// LIB:          let {{.*}} = E::host_scale({{.*}});
// LIB:      pub fn twice<E: Externals>(v: i32) -> i32 {
// LIB:          let {{.*}} = scaled::<E>({{.*}});
// A function that reaches no requirement keeps its exact signature.
// LIB:      pub fn untouched(a: i32, b: i32) -> i32 {

// The SAME project with an entry point is a binary crate under `auto`, and a
// binary crate has no caller to supply the impl -- so the historical
// whole-program error is unchanged.
// RUN: not emitrust-cc --emit=rust %t/program.c -o - 2>&1 \
// RUN:   | FileCheck --check-prefix=BIN %s
// BIN: error: unsupported: function 'host_scale' is referenced but not defined in any translation unit

// `--crate-type=lib` is the escape hatch: the user has said the output is a
// library, so `c_main` is just another exported (and here generic) function.
// RUN: emitrust-cc --emit=rust --crate-type=lib %t/program.c -o - \
// RUN:   | FileCheck --check-prefix=FORCED %s
// FORCED:      pub trait Externals {
// FORCED-NEXT:     fn host_scale(v0: i32) -> i32;
// FORCED-NEXT: }
// FORCED:      pub fn c_main<E: Externals>() -> i32 {

// `--crate-type=bin` says the opposite, and is refused rather than silently
// answered with something else.
// RUN: not emitrust-cc --emit=rust --crate-type=bin %t/program.c -o - 2>&1 \
// RUN:   | FileCheck --check-prefix=BIN %s

// Taking the ADDRESS of a function the trait made generic is an edge too: the
// emitted `Some(f)` names the item directly, so it must become
// `Some(f::<E>)` and its enclosing function must acquire the parameter. A
// function pointer to code that needs nothing is left alone, and the callee
// that merely INVOKES the pointer stays non-generic -- the pointer value is
// already monomorphized by the time it is passed.
// RUN: emitrust-cc --emit=rust %t/fnptr.c -o - | FileCheck --check-prefix=FNPTR %s
// FNPTR:      pub fn apply(v0: Option<fn(i32) -> i32>, v: i32) -> i32 {
// FNPTR:      pub fn run<E: Externals>(v: i32) -> i32 {
// FNPTR:          let {{.*}}: Option<fn(i32) -> i32> = Some(scaled::<E>);
// FNPTR:          let {{.*}}: Option<fn(i32) -> i32> = Some(plain);

//--- library.c
int host_scale(int v);

int scaled(int v) { return host_scale(v) + 1; }
int twice(int v) { return scaled(v) + scaled(v); }
int untouched(int a, int b) { return a + b; }

//--- program.c
int host_scale(int v);

int scaled(int v) { return host_scale(v) + 1; }

int main(void) { return scaled(2); }

//--- fnptr.c
int host_scale(int v);

int scaled(int v) { return host_scale(v) + 1; }
int plain(int v) { return v * 2; }

int apply(int (*f)(int), int v) { return f(v); }

int run(int v) {
  int (*p)(int) = scaled;
  int (*q)(int) = plain;
  return apply(p, v) + apply(q, v);
}

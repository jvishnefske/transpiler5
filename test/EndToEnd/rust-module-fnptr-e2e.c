// REQUIRES: cargo
// FR-159 phase 1: the byte-diff oracle for MODULE-rendered items. The
// emitted crate is built with rustc and RUN, and its stdout is diffed
// against the clang-linked native binary of the same program.
//
// Two properties of the module rendering cannot be seen by FileCheck on
// emitted text, and both are measured defects the phase-1 spike found:
//   * a fn-ptr payload spelled RELATIVELY (`Some(tu0::add1)`) is rustc
//     E0433 inside `mod tu0`, because paths in a module are
//     module-relative. Payloads must be ABSOLUTE (`Some(crate::tu0::add1)`),
//     and a fn-ptr TABLE renders inside the module, so this is the common
//     case;
//   * the FR-77 dangling-target backstop and the FR-63 `const { ... }`
//     decision both parsed payload text as a bare identifier and silently
//     no-opped on anything containing ':'. A table that quietly lost its
//     wrapper still compiles; a dangling target that quietly lost its
//     diagnostic still compiles too, and then dies as rustc E0425 with no
//     source location. Only running the crate proves the dispatch actually
//     reaches the function each path names.
//
// The two TUs' statics share every spelling (`add1`, `table`, `total`) and
// hold different values, so any crossing of the two module paths shows up
// as a different number on stdout. Every value derives from argc.
//
// The MLIR fixture is hand-authored (see its header): phase 1 sinks only
// records, so no C input reaches the module shape yet. The `.c` sources
// here and in Inputs/ are the oracle.
//
// RUN: emitrust-translate --mlir-to-rust %S/Inputs/rust-module-fnptr.mlir -o %t.rs
// RUN: FileCheck %s --check-prefix=EMITTED --strict-whitespace < %t.rs
// EMITTED:      mod tu0 {
// EMITTED-NEXT:     pub(crate) fn add1(v: i32) -> i32 {
// The table payload is ABSOLUTE and keeps its const-block wrapper:
// EMITTED:          pub(crate) static TABLE: std::cell::Cell<[Option<fn(i32) -> i32>; 2]> = const { std::cell::Cell::new([Some(crate::tu0::add1), Some(crate::tu0::add2)]) };
// EMITTED:      mod tu1 {
// EMITTED:          pub(crate) static TABLE: std::cell::Cell<[Option<fn(i32) -> i32>; 1]> = const { std::cell::Cell::new([Some(crate::tu1::add1)]) };
//
// RUN: rustc --edition=2021 --crate-name mod_fnptr -A dead_code -o %t.rust %t.rs
// RUN: clang -std=c11 %s %S/Inputs/rust-module-fnptr-h1.c %S/Inputs/rust-module-fnptr-h2.c -o %t.native
// RUN: %t.native a b c > %t.native.out
// RUN: %t.rust a b c > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// ... and at a second argc, so one accidental agreement cannot pass for
// equality (the accumulators are stateful, so this also re-runs from a
// fresh initial TOTAL):
// RUN: %t.native > %t.native1.out
// RUN: %t.rust > %t.rust1.out
// RUN: diff %t.native1.out %t.rust1.out

int printf(const char *, ...);
int drive1(int, int);
int drive2(int);

int main(int argc, char **argv) {
  printf("%d %d %d\n", drive1(0, argc), drive1(1, argc + 1), drive2(argc + 4));
  return 0;
}

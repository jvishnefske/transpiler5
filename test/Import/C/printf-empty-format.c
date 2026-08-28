// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=NOBARE

// FR-131: the EMPTY printf format string. `emitPrintMacro` folds a trailing
// newline into the `ln` macro variant and then renders a leftover-empty
// format from an EMPTY args array, so it spells `println!()` rather than
// `println!("")` (which would trip clippy::println_empty_string). That
// shortcut is only sound when the emptiness is the RESIDUE of the fold: a
// format that was empty to begin with keeps the non-`ln` macro, and
// `print!()` is not valid Rust -- rustc rejects it with "requires at least a
// format string argument" and the whole crate fails to build (exit-0
// unbuildable). This file pins BOTH halves so neither can drift:
//   * `printf("")` keeps its literal and emits `print!("")` (zero bytes, the
//     same as C, and measured to trip no clippy lint under
//     clippy::all + clippy::pedantic);
//   * `printf("\n")` still emits the BARE `println!()` -- the byte-identity
//     half; a too-broad gate would shift it to `println!("")`.

int printf(const char *fmt, ...);

// CHECK-LABEL: func.func @c_main() -> i32
int main(void) {
  // The empty format keeps a (zero-length) format literal in the args array.
  // CHECK: emitrust.call_opaque "print!"() {args = [""]} : () -> ()
  printf("");

  // The fold residue keeps the bare form: an EMPTY args array.
  // CHECK: emitrust.call_opaque "println!"() {args = []} : () -> ()
  printf("\n");

  // A non-empty format is unaffected either way.
  // CHECK: emitrust.call_opaque "print!"() {args = ["x"]} : () -> ()
  printf("x");
  // CHECK: emitrust.call_opaque "println!"() {args = ["y"]} : () -> ()
  printf("y\n");

  return 0;
}

// The invalid `print!()` spelling (an empty args array on the non-`ln`
// macro) must appear nowhere in the module -- that is the exact byte rustc
// rejects.
// NOBARE-NOT: emitrust.call_opaque "print!"() {args = []}
// NOBARE-NOT: emitrust.call_opaque "eprint!"() {args = []}

// FR-20: emitrust-cc --emit=rust prints the exact text of the crate's
// src/main.rs when the input defines main: allow-header, translated
// functions (C main renamed to c_main), and the process-exit wrapper.
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

int printf(const char *, ...);

int twice(int x) { return x + x; }

int main(void) {
  int total = 0;
  for (int i = 0; i < 4; ++i) {
    total = total + twice(i);
  }
  printf("total=%d\n", total);
  return 0;
}

// The allow-header comes first: only dead_code and unused_assignments remain
// allowed (both intrinsic to a faithful transpile); every other lint is denied
// in Cargo.toml.
// CHECK: #![allow(dead_code, unused_assignments)]

// The helper function keeps its name; C main is renamed to c_main.
// CHECK: fn twice(v0: i32) -> i32 {
// CHECK: fn c_main() -> i32 {

// The printf call renders as a print! macro invocation inside c_main; the
// function-final return renders as a tail expression (FR-61a), so no
// `return` statement appears.
// CHECK: print!("total={}
// CHECK: v2

// The verbatim wrapper forwards c_main's result as the exit code.
// CHECK: fn main() { std::process::exit(c_main()); }

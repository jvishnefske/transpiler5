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

// The allow-header comes first: the emitter's statement-per-op, mut-let
// style legitimately triggers these lints.
// CHECK: #![allow(unused_variables, unused_assignments, unused_mut, unused_parens, dead_code)]

// The helper function keeps its name; C main is renamed to c_main.
// CHECK: fn twice(v0: i32) -> i32 {
// CHECK: fn c_main() -> i32 {

// The printf call renders as a print! macro invocation inside c_main.
// CHECK: print!("total={}
// CHECK: return

// The verbatim wrapper forwards c_main's result as the exit code.
// CHECK: fn main() { std::process::exit(c_main()); }

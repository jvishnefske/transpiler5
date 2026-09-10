// FR-20: emitrust-cc --emit=rust prints the exact text of the crate's
// src/main.rs when the input defines main: the translated functions (C main
// renamed to c_main) and the process-exit wrapper.
//
// FR-220 moved what used to be the file's first line. `dead_code` was the last
// lint still allowed crate-wide, and a crate-root allow is a blindfold: it hid
// every kind of dead item at once, including the ONE kind that signals an
// emitter defect (a dead emitted `fn`). The allow now rides on the individual
// records, enums and inherent impls the emitter knows it is writing, so this
// input -- which declares no type at all -- gets NO attribute header, and the
// crate root's first byte is its first emitted item. The NOALLOW run below
// states that as a sweep over the whole output rather than only the prefix,
// so reinstating a blanket `#![allow(..)]` anywhere fails this test.
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s --check-prefix=NOALLOW

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

// There is no attribute header at all: not one crate-level allow survives, and
// every lint the old blanket header silenced is denied in Cargo.toml.
// NOALLOW-NOT: #![allow

// The helper function keeps its name; C main is renamed to c_main. It is the
// FIRST line of the file now that the header is gone.
// CHECK: fn twice(x: i32) -> i32 {
// CHECK: fn c_main() -> i32 {

// The printf call renders as a print! macro invocation inside c_main; the
// function-final return renders as a tail expression (FR-61a) -- since
// FR-61d slice 2 the returned multi-use zero constant duplicates as a
// suffixed literal, so the tail is the literal itself -- and no `return`
// statement appears.
// CHECK: println!("total={}
// CHECK: 0i32

// The verbatim wrapper forwards c_main's result as the exit code.
// CHECK: fn main() { std::process::exit(c_main()); }

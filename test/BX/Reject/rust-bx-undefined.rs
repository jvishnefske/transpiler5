// The safe `bx::` helpers are ordinary Rust items: a crate that calls one
// without defining it in `mod bx` does not compile (rustc E0433). The parser
// once accepted such input and printed C++ for a program rustc rejects, so the
// Rust subset now requires every helper it uses to be defined -- located at
// the use, never silently accepted.
// RUN: not %emitrust-bx %s 2>&1 | FileCheck %s
fn main() {
    let mut s: String = String::new();
    // CHECK: rust-bx-undefined.rs:[[@LINE+1]]:5: error: unsupported: `bx::push_byte` without its definition in `mod bx` (rustc E0433)
    bx::push_byte(&mut s, 65i8);
    print!("{}\n", s);
}

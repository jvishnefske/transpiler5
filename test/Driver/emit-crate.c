// FR-20: emitrust-cc --emit=crate writes a complete cargo crate: a minimal
// Cargo.toml whose package name is the sanitized input basename
// (emit-crate.c -> emit_crate), and src/main.rs with allow-header,
// translation, and the fn main wrapper.
// RUN: emitrust-cc --emit=crate %s -o %t.crate
// RUN: cat %t.crate/Cargo.toml | FileCheck %s --check-prefix=TOML
// RUN: cat %t.crate/src/main.rs | FileCheck %s --check-prefix=MAIN

int add(int a, int b) { return a + b; }

int main(void) { return add(20, 22) - 42; }

// TOML:      [package]
// TOML-NEXT: name = "emit_crate"
// TOML-NEXT: version = "0.1.0"
// TOML-NEXT: edition = "2021"

// MAIN: #![allow(unused_variables, unused_assignments, unused_mut, unused_parens, dead_code)]
// MAIN: fn add(v0: i32, v1: i32) -> i32 {
// MAIN: fn c_main() -> i32 {
// MAIN: fn main() { std::process::exit(c_main()); }

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
// The lints the old blanket allow-header silenced are now denied so a
// regression fails the build; dead_code and unused_assignments stay allowed.
// TOML:      [lints.rust]
// TOML-NEXT: unused_variables = "deny"
// TOML-NEXT: unused_mut = "deny"
// TOML-NEXT: unused_parens = "deny"
// TOML-NEXT: unpredictable_function_pointer_comparisons = "deny"
// TOML-NEXT: non_snake_case = "deny"
// TOML-NEXT: non_upper_case_globals = "deny"
// TOML-NEXT: non_camel_case_types = "deny"

// MAIN: #![allow(dead_code, unused_assignments)]
// MAIN: fn add(v0: i32, v1: i32) -> i32 {
// MAIN: fn c_main() -> i32 {
// MAIN: fn main() { std::process::exit(c_main()); }

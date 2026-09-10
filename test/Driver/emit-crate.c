// FR-20: emitrust-cc --emit=crate writes a complete cargo crate: a minimal
// Cargo.toml whose package name is the sanitized input basename
// (emit-crate.c -> emit_crate), and src/main.rs with the translation and the
// fn main wrapper.
//
// FR-220: src/main.rs no longer opens with an attribute header. `dead_code`
// was the last blanket crate-root allow and it moved onto the individual items
// (records, enums, inherent impls) that legitimately produce it; a plain `fn`
// is deliberately left uncovered, so a dead emitted function -- the one kind
// that would indicate an emitter defect -- becomes visible instead of hidden.
// This input declares no type, so its crate root starts at its first item.
// RUN: emitrust-cc --emit=crate %s -o %t.crate
// RUN: cat %t.crate/Cargo.toml | FileCheck %s --check-prefix=TOML
// RUN: cat %t.crate/src/main.rs | FileCheck %s --check-prefix=MAIN
// RUN: cat %t.crate/src/main.rs | FileCheck %s --check-prefix=NOALLOW

int add(int a, int b) { return a + b; }

int main(void) { return add(20, 22) - 42; }

// TOML:      [package]
// TOML-NEXT: name = "emit_crate"
// TOML-NEXT: version = "0.1.0"
// TOML-NEXT: edition = "2021"
// The lints the old blanket allow-header silenced are now denied so a
// regression fails the build. `dead_code` is deliberately NOT in this table:
// it was measured unlandable as a deny (an external-linkage C function
// uncalled in its own TU is not even a C warning, and recovery structurally
// manufactures dead functions by dropping a rejected CALLER), so FR-220's
// tripwire is the clippy-eval ratchet, which counts `dead_code` at zero over
// the epoch, rather than a hard error in every emitted crate.
// TOML:      [lints.rust]
// TOML-NEXT: unused_variables = "deny"
// TOML-NEXT: unused_assignments = "deny"
// TOML-NEXT: unused_mut = "deny"
// TOML-NEXT: unused_parens = "deny"
// TOML-NEXT: unpredictable_function_pointer_comparisons = "deny"
// TOML-NEXT: non_snake_case = "deny"
// TOML-NEXT: non_upper_case_globals = "deny"
// TOML-NEXT: non_camel_case_types = "deny"
// ... and nothing about `dead_code` follows it, in either direction.
// TOML-NOT:  dead_code

// No crate-root attribute header survives anywhere in src/main.rs; the first
// byte of the file is the first emitted item.
// NOALLOW-NOT: #![allow
// MAIN: fn add(a: i32, b: i32) -> i32 {
// MAIN: fn c_main() -> i32 {
// MAIN: fn main() { std::process::exit(c_main()); }

// FR-51: the other half of the crate-shape rule. This input DOES define
// `main`, so every default path must keep emitting exactly the binary crate
// it always has, and --crate-type=lib must be able to override that.
//
// RUN: emitrust-cc --emit=crate %s -o %t.auto
// RUN: cat %t.auto/Cargo.toml | FileCheck %s --check-prefix=BINTOML
// RUN: ls %t.auto/src | FileCheck %s --check-prefix=BINLAYOUT
// RUN: cat %t.auto/src/main.rs | FileCheck %s --check-prefix=BIN
//
// Explicit --crate-type=bin and --crate-type=auto agree with the default,
// byte for byte, on both files.
// RUN: emitrust-cc --emit=crate --crate-type=bin %s -o %t.bin
// RUN: diff %t.auto/Cargo.toml %t.bin/Cargo.toml
// RUN: diff %t.auto/src/main.rs %t.bin/src/main.rs
// RUN: emitrust-cc --emit=crate --crate-type=auto %s -o %t.autoexp
// RUN: diff %t.auto/Cargo.toml %t.autoexp/Cargo.toml
// RUN: diff %t.auto/src/main.rs %t.autoexp/src/main.rs
//
// A binary crate exports NOTHING: its items are private and the
// #![allow(dead_code)] header is what keeps that warning-clean. This is the
// property that makes FR-51 byte-identical for every pre-existing input.
// RUN: not grep "pub " %t.auto/src/main.rs
//
// --crate-type=lib overrides the choice: `c_main` stops being an entry point
// and becomes an ordinary exported function, and no wrapper is written.
// RUN: emitrust-cc --emit=crate --crate-type=lib %s -o %t.forcedlib
// RUN: cat %t.forcedlib/src/lib.rs | FileCheck %s --check-prefix=FORCEDLIB
// RUN: cat %t.forcedlib/Cargo.toml | FileCheck %s --check-prefix=LIBTOML

int printf(const char *, ...);

static int helper(int x) { return x * 2; }

int doubled(int x) { return helper(x); }

int main(void) {
  printf("%d\n", doubled(21));
  return 0;
}

// No [lib] section for a binary crate: the manifest is byte-for-byte the
// pre-FR-51 one.
// BINTOML:      [package]
// BINTOML-NEXT: name = "emit_crate_type_bin"
// BINTOML-NEXT: version = "0.1.0"
// BINTOML-NEXT: edition = "2021"
// BINTOML:      [lints.rust]
// BINTOML-NEXT: unused_variables = "deny"
// BINTOML-NOT:  [lib]

// BINLAYOUT-NOT: lib.rs
// BINLAYOUT: main.rs
// BINLAYOUT-NOT: lib.rs

// BIN: #![allow(dead_code, unused_assignments)]
// BIN: fn tu0_helper(x: i32) -> i32 {
// BIN: fn doubled(x: i32) -> i32 {
// BIN: fn c_main() -> i32 {
// BIN: fn main() { std::process::exit(c_main()); }

// The forced library keeps the same visibility rule: the file-static stays
// private, and everything with external linkage -- `c_main` included, since
// C gave it external linkage -- is exported.
// FORCEDLIB: fn tu0_helper(
// FORCEDLIB: pub fn doubled(
// FORCEDLIB: pub fn c_main(
// FORCEDLIB-NOT: fn main() { std::process::exit

// LIBTOML: [lib]
// LIBTOML-NEXT: name = "emit_crate_type_bin"

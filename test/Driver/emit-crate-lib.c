// FR-51: emitrust-cc emits a LIBRARY crate for input that defines no `main`,
// and a binary crate for input that does. The shape is chosen from the module
// and can be forced with --crate-type.
//
// This input has no `main`, so it is the library case.
// RUN: emitrust-cc --emit=crate %s -o %t.lib
// RUN: cat %t.lib/Cargo.toml | FileCheck %s --check-prefix=LIBTOML
// RUN: ls %t.lib/src | FileCheck %s --check-prefix=LIBLAYOUT
// RUN: cat %t.lib/src/lib.rs | FileCheck %s --check-prefix=LIB
// RUN: cat %t.lib/src/lib.rs | FileCheck %s --check-prefix=NOALLOW
//
// --emit=rust prints exactly that crate root (FR-51 makes the invariant
// total; before it, a no-`main` input printed a bare, header-less
// translation because there was no library crate for it to be the root of).
// RUN: emitrust-cc --emit=rust %s -o %t.rust
// RUN: diff %t.lib/src/lib.rs %t.rust
//
// Forcing a binary crate on input with no entry point is a located error, not
// a crate that cannot link.
// RUN: not emitrust-cc --emit=crate --crate-type=bin %s -o %t.forcedbin 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FORCEBIN
//
// --crate-type=auto is the default and must reproduce it byte for byte.
// RUN: emitrust-cc --emit=crate --crate-type=auto %s -o %t.autolib
// RUN: diff %t.lib/src/lib.rs %t.autolib/src/lib.rs
// RUN: diff %t.lib/Cargo.toml %t.autolib/Cargo.toml
//
// --crate-type is about crate SHAPE, so it is rejected where no crate root is
// produced.
// RUN: not emitrust-cc --emit=mlir --crate-type=lib %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=BADFLAG

struct Point {
  int x;
  int y;
};

enum Axis { AXIS_X, AXIS_Y };

// Internal linkage: TU-private by the author's choice, and it must STAY
// private in the emitted library. The importer tags it `tu<i>_`, which is the
// only trace of its linkage that survives into the module, and the emitter
// reads that tag back (EmitRust/CSymbolLinkage.h).
static int clamp_low(int v) { return v < 0 ? 0 : v; }

// External linkage: this is the library's API and must be reachable from
// outside the crate.
int point_manhattan(struct Point p) {
  return clamp_low(p.x) + clamp_low(p.y);
}

int axis_of(int which) { return which == 0 ? AXIS_X : AXIS_Y; }

// The manifest gains a [lib] section; the [package] table is unchanged.
// LIBTOML:      [package]
// LIBTOML-NEXT: name = "emit_crate_lib"
// LIBTOML-NEXT: version = "0.1.0"
// LIBTOML-NEXT: edition = "2021"
// LIBTOML:      [lib]
// LIBTOML-NEXT: name = "emit_crate_lib"
// LIBTOML-NEXT: path = "src/lib.rs"
// The [lints.rust] deny table follows the [lib] section, same as for a binary.
// LIBTOML:      [lints.rust]
// LIBTOML-NEXT: unused_variables = "deny"
// LIBTOML-NEXT: unused_assignments = "deny"
// LIBTOML-NEXT: unused_mut = "deny"
// LIBTOML-NEXT: unused_parens = "deny"
// LIBTOML-NEXT: unpredictable_function_pointer_comparisons = "deny"
// LIBTOML-NEXT: non_snake_case = "deny"
// LIBTOML-NEXT: non_upper_case_globals = "deny"
// LIBTOML-NEXT: non_camel_case_types = "deny"
// `dead_code` is NOT in the table: FR-220 measured `dead_code = "deny"` as
// unlandable (it regresses c-testsuite by 10 and breaks FR-44's recovery
// guarantee), so the tripwire is the clippy-eval ratchet, not a hard error.
// LIBTOML-NOT:  dead_code

// The crate root is src/lib.rs, and there is no src/main.rs at all.
// LIBLAYOUT-NOT: main.rs
// LIBLAYOUT: lib.rs
// LIBLAYOUT-NOT: main.rs

// FR-220: there is no crate-level attribute header, for a library exactly as
// for a binary. `dead_code` was the last blanket allow and it moved onto the
// items -- here the `struct Point`, the transparent `struct Axis` and its
// associated-constant `impl` each carry their own -- so the crate root's first
// byte is that first attribute rather than a `#![..]` inner attribute. The
// exports below are unchanged; what moved is attribute POSITION only.
// LIB:      #[allow(dead_code)]
// NOALLOW-NOT: #![allow

// Types are exported unconditionally, with their fields and variant
// constants: Rust's private-in-public rule (E0446) means a type named in an
// exported signature must itself be exported, and a C record definition
// carries no linkage of its own to leak.
// LIB: pub struct Point {
// LIB-DAG: pub x: i32,
// LIB-DAG: pub y: i32,
// LIB: pub struct Axis(pub u32);
// LIB: pub const AXIS_X: Axis
// A trait impl's method must NOT be `pub` -- its visibility comes from the
// trait -- so the enum's synthesized Default stays bare.
// LIB: impl Default for Axis {
// LIB-NEXT: fn default()

// The file-static stays private; every external-linkage function is pub.
// LIB: fn tu0_clamp_low(
// LIB: pub fn point_manhattan(
// LIB: pub fn axis_of(

// A library crate gets no entry-point wrapper.
// LIB-NOT: fn main()

// FORCEBIN: error: cannot emit a binary crate: the input does not define a 'main' function (imported as 'c_main'). Drop --crate-type=bin to emit a library crate instead

// BADFLAG: error: --crate-type is only valid with --emit=crate or --emit=rust

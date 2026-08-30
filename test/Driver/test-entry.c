// FR-160: `--test-entry` turns a C project's own test entry points into
// generated `#[test]` functions, so `cargo test` on an emitted crate is a
// DIFFERENTIAL oracle -- the C suite's own assertions, checked against the
// transpiled code, over paths no hand-written EndToEnd test will ever cover.
//
// The pass criterion is inherited, not invented: every meson test and every
// CTest test declares that it passes iff the binary exits 0, so an entry
// returning an integer becomes `assert_eq!(sym(), 0)`. An entry returning
// nothing is run for its panics -- a weaker oracle but not a vacuous one,
// because a transpiled body's failure mode IS a panic (a bounds check, a null
// function pointer, an `unimplemented!`).
//
// This file pins the whole policy, whose governing rule is NEVER A VACUOUS
// PASS:
//
// 1. An importable no-arg entry is wrapped, in both the integer and the void
//    shape.
//
// 2. A RECOVERED STUB is wrapped and `#[ignore]`d WITH ITS DIAGNOSTIC. It
//    would panic if run, so it must not count as passing; but deleting it
//    would hide the gap, and `cargo test` naming an ignored test with the
//    reason is how the gap stays visible.
//
// 3. An entry that CANNOT be called is not wrapped at all and says why, with
//    a location: one that takes arguments (nothing models argv, and meson's
//    own registry says 333 of systemd's 337 one-TU tests take none), and one
//    that is generic over the `Externals` trait, whose callees are undefined
//    in a solo-TU import -- a test that can only panic inside a trait method
//    proves nothing. `--link` the shards and both the trait and the
//    restriction disappear.
//
// 4. A symbol this module does not define is reported when it was named by
//    hand, because the caller asked for it and got nothing.
//
// 5. WITHOUT the flag not one byte moves. That is why it defaults off, and
//    the no-flag run is checked here explicitly rather than assumed.
//
// RUN: emitrust-cc --emit=rust --crate-type=lib --recover %s \
// RUN:   --test-entry=exit_code --test-entry=returns_nothing \
// RUN:   --test-entry=stubbed --test-entry=needs_args \
// RUN:   --test-entry=calls_out --test-entry=not_here \
// RUN:   -o %t.rs 2>%t.err
// RUN: FileCheck %s --check-prefix=TESTS --input-file=%t.rs
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
//
// The skipped entries are skipped -- no test names them at all.
// RUN: not grep 'fn needs_args()' %t.rs
// RUN: not grep 'fn calls_out()' %t.rs
// RUN: not grep 'fn not_here()' %t.rs
//
// The byte-identity guard: the same input with no entry requested is exactly
// today's output, and silent.
// RUN: emitrust-cc --emit=rust --crate-type=lib --recover %s -o %t.plain.rs \
// RUN:   2>%t.plain.err
// RUN: not grep 'cfg(test)' %t.plain.rs
// RUN: not grep FR-160 %t.plain.err
// The flagged output's first N lines ARE the unflagged output, byte for
// byte; everything the flag adds comes after them.
// RUN: head -n `wc -l < %t.plain.rs` %t.rs | diff %t.plain.rs -

#include <strings.h>

int exit_code(void) { return 0; }

void returns_nothing(void) {}

int stubbed(void) {
  char a[4] = "ab", b[4] = "ab";
  return strcasecmp(a, b);
}

int needs_args(int x) { return x; }

int helper(int x);
int calls_out(void) { return helper(1); }

// The generated module is one `#[cfg(test)]` block at the end of the crate
// root, allowing `non_snake_case` because a C test symbol need not be snake
// case -- scoped to the generated module, so the manifest's deny still governs
// every item the emitter itself writes.
// TESTS:      #[cfg(test)]
// TESTS-NEXT: #[allow(non_snake_case)]
// TESTS-NEXT: mod emitrust_tests {
//
// TESTS:      #[test]
// TESTS-NEXT: fn exit_code() {
// TESTS-NEXT: assert_eq!(super::exit_code(), 0);
//
// TESTS:      #[test]
// TESTS-NEXT: fn returns_nothing() {
// TESTS-NEXT: super::returns_nothing();
//
// TESTS:      #[test]
// TESTS-NEXT: #[ignore = "unsupported: call to 'strcasecmp'
// TESTS-NEXT: fn stubbed() {
//
// WARN: warning: FR-160: no test emitted for 'needs_args': takes arguments
// WARN: warning: FR-160: no test emitted for 'calls_out': generic over the Externals trait
// WARN: warning: FR-160: no test emitted for 'not_here': no function of that name in this crate

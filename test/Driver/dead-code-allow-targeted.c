// FR-220 TRIPWIRE, emit half: `dead_code` is allowed on the ITEM KINDS that
// legitimately produce it and on NOTHING else -- above all, never on a `fn`
// and never at the crate root.
//
// The invariant this file pins, and why it matters. `dead_code` was the last
// lint the emitted crate silenced with a blanket `#![allow(..)]` in its crate
// root. A crate-root allow is a blindfold: it hides every kind of dead item at
// once, including the ONE kind that would signal an emitter defect. Measured
// over the whole epoch-7 population (294 crates) with the blanket allow
// stripped: 33 crates warn, 56 warnings, and ZERO of them are `function X is
// never used`. Every warning that fires is faithful translation of something
// the C program declared and did not use -- an unmentioned enumerator, an
// unread field, an unconstructed record, an uncalled method -- and no C
// compiler warns about any of them either.
//
// So the allow moved onto those four item kinds, one attribute each, and a
// plain `fn` deliberately gets NONE. This input carries all five shapes at
// once:
//   * `struct never_built`  -- a record the TU declares and never constructs;
//   * `enum code`           -- lowered to a transparent newtype AND a sibling
//                              `impl` of associated constants, which need
//                              SEPARATE attributes because an attribute on
//                              the struct does not reach the sibling impl;
//                              `CODE_LAST_UNUSED` is never mentioned;
//   * the FR-62 actor struct + its INHERENT impl lifted from `static g_total`;
//   * `static never_called` -- a dead FUNCTION, which must come out BARE.
//
// Fidelity is the point: every one of these items is still EMITTED. FR-220
// attributes differently, it does not drop anything. The `IMPL` and `BARE`
// sweeps below say the two halves of that directly.
//
// The rustc half of the tripwire -- that the bare `fn` really does draw a
// `dead_code` diagnostic, and that the covered items really do not -- needs
// cargo and lives in test/Driver/dead-code-tripwire.c.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate
// RUN: FileCheck %s --check-prefix=RUST --strict-whitespace \
// RUN:   < %t.crate/src/main.rs
// RUN: FileCheck %s --check-prefix=NOBLANKET < %t.crate/src/main.rs
// RUN: FileCheck %s --check-prefix=TOML < %t.crate/Cargo.toml
//
// Under --preserve-c-names the THREE NAMING lints keep their crate-root allow
// -- verbatim C spellings legitimately trip them and keeping those spellings
// is the flag's whole point -- and `dead_code` is NOT among them.
// RUN: emitrust-cc --emit=rust --preserve-c-names %s -o - \
// RUN:   | FileCheck %s --check-prefix=CNAMES --strict-whitespace

int printf(const char *, ...);

struct never_built { int a; int b; };

enum code { CODE_OK, CODE_LAST_UNUSED };

static int g_total;

static int never_called(int x) { return x + 1; }

void bump(int n) { g_total += n; }

int main(void) {
  bump(3);
  printf("%d %d\n", g_total, (int)CODE_OK);
  return 0;
}

// The crate root's first byte is its first emitted item: no attribute header.
// RUST:      #[allow(dead_code)]
// RUST-NEXT: #[derive(Clone, Copy, Default)]
// RUST-NEXT: struct NeverBuilt {
// RUST-NEXT:     a: i32,
// RUST-NEXT:     b: i32,
// RUST-NEXT: }
// The enum's TWO items each carry their own attribute. `impl Default` carries
// none, and needs none: rustc's dead-code pass never reports trait-impl
// members, because the trait is the use site.
// RUST-NEXT: #[repr(transparent)]
// RUST-NEXT: #[derive(Clone, Copy, PartialEq)]
// RUST-NEXT: #[allow(dead_code)]
// RUST-NEXT: struct Code(u32);
// RUST-NEXT: #[allow(dead_code)]
// RUST-NEXT: impl Code {
// RUST-NEXT:     const CODE_OK: Code = Code(0);
// RUST-NEXT:     const CODE_LAST_UNUSED: Code = Code(1);
// RUST-NEXT: }
// RUST-NEXT: impl Default for Code {
// RUST-NEXT:     fn default() -> Code { Code::CODE_OK }
// RUST-NEXT: }
// The FR-62 actor record and its INHERENT impl.
// RUST-NEXT: #[allow(dead_code)]
// RUST-NEXT: #[derive(Clone, Copy, Default)]
// RUST-NEXT: struct Tu0GTotalActor {
// RUST-NEXT:     tu0_g_total: i32,
// RUST-NEXT: }
// RUST-NEXT: #[allow(dead_code)]
// RUST-NEXT: impl Tu0GTotalActor {
// RUST-NEXT:     fn bump(&mut self, n: i32) {
// RUST-NEXT:         self.tu0_g_total += n;
// RUST-NEXT:     }
// RUST-NEXT: }
// ... and the dead FUNCTION follows it BARE. It is still emitted -- fidelity
// is preserved, the attribution changed -- and it is the tripwire: nothing
// covers it, so rustc can still see it.
// RUST-NEXT: fn tu0_never_called(x: i32) -> i32 {
// RUST-NEXT:     x + 1i32
// RUST-NEXT: }
// RUST-NEXT: fn c_main() -> i32 {

// Not one crate-level allow anywhere in the file. Reinstating the blanket
// header -- which would delete the tripwire, the entire point of FR-220 --
// fails here.
// NOBLANKET-NOT: #![allow

// `dead_code` is deliberately absent from the deny table too. Denying it was
// MEASURED and rejected twice over: it regresses the c-testsuite ledger by 10
// (an external-linkage C function uncalled in its own TU is not even a C
// warning; the deadness is an artifact of the emitter privatizing it for a
// `bin` crate), and it breaks FR-44's headline guarantee, because recovery
// drops a rejected CALLER and orphans every function only that caller reached
// -- recovery STRUCTURALLY manufactures dead functions. The tripwire is the
// clippy-eval ratchet, which counts `dead_code` and holds the epoch at zero.
// TOML:      [lints.rust]
// TOML-NEXT: unused_variables = "deny"
// TOML-NOT:  dead_code

// --preserve-c-names: exactly the three naming lints, verbatim, and nothing
// else. The per-item `dead_code` allows are emitted here too -- they are the
// emitter's, not the header's -- and the C spellings are untouched.
// CNAMES:      #![allow(non_snake_case, non_upper_case_globals, non_camel_case_types)]
// CNAMES-EMPTY:
// CNAMES-NEXT: #[allow(dead_code)]
// CNAMES-NEXT: #[derive(Clone, Copy, Default)]
// CNAMES-NEXT: struct never_built {

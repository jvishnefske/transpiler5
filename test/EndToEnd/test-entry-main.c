// REQUIRES: cargo
// FR-160b end to end: the `main` -> `c_main` alias wraps a REAL binary, and it
// can go red.
//
// This is the motivating path of the whole FR, and before this wave it was
// silent. `scripts/test-entries-meson.py`'s default mode writes the literal
// symbol `main` into the registry for every one-TU test -- 337 of them on
// systemd -- while the emitter renames `main` to `c_main` UNCONDITIONALLY
// (`--preserve-c-names` does not turn this one off). Fed back through
// `--test-entries`, the FR's own Phase B output produced ZERO tests and ZERO
// diagnostics against its own Phase A: a whole project's suite silently
// yielding no coverage at all.
//
// `cargo build` success cannot see a miscompile, so this file leans on the two
// oracles that can:
//
// 1. The emitted binary's stdout, byte-diffed against the clang-built native.
//    The seed CANNOT come from `argc` here, unlike every neighbouring
//    EndToEnd test: a wrappable entry point takes no arguments -- that is
//    FR-160's own policy, and a `main(int, char **)` is skipped "takes
//    arguments" instead -- so the subject has to be `main(void)`. Its
//    arithmetic runs through a mutating loop over a file-scope array, and the
//    printed values are the loop's, so a wrong lowering of the state the actor
//    machinery moves still shows up in the diff.
//
// 2. `cargo test` on the emitted crate, both ways round: a C `main` returning 0
//    is a test that PASSES, and a C `main` returning nonzero is a test that
//    FAILS. This pair carries the non-vacuity the missing `argc` seed would
//    otherwise carry -- the alias must preserve the pass criterion meson and
//    CTest already agree on (a test passes iff it exits 0), and a wrap that
//    could only ever go green would fail the second half.
//
// The wrap lands inside `mod emitrust_tests`, so `#[test] fn main()` coexists
// with the crate's own `fn main()` shim; the module-scoped
// `#[allow(non_snake_case)]` the emitter already writes covers a C spelling
// that is not snake case.
//
// RUN: echo 'main # exactly what scripts/test-entries-meson.py writes' > %t.entries
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name te_main \
// RUN:   --build --test-entries=%t.entries 2>%t.err
// RUN: not grep FR-160 %t.err
// RUN: grep 'assert_eq!(super::c_main(), 0);' %t.crate/src/main.rs
//
// Oracle 1 -- the emitted binary against the clang native, argc-seeded.
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/te_main > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// Oracle 2a -- the generated test actually runs, and passes.
// RUN: cargo test --offline --release --manifest-path %t.crate/Cargo.toml 2>&1 \
// RUN:   | FileCheck %s --check-prefix=PASS
// PASS: test emitrust_tests::main ... ok
//
// Oracle 2b -- and it can go red. The C entry exits nonzero, the native run
// exits nonzero, and so does the generated test.
// RUN: echo 'main' > %t.fail.entries
// RUN: emitrust-cc --emit=crate %S/Inputs/test-entry-main/failing-main.c \
// RUN:   -o %t.fcrate --crate-name te_main_fail --build \
// RUN:   --test-entries=%t.fail.entries 2>%t.ferr
// RUN: not grep FR-160 %t.ferr
// RUN: clang -std=c11 %S/Inputs/test-entry-main/failing-main.c -o %t.fnative
// RUN: not %t.fnative > %t.fnative.out
// RUN: not %t.fcrate/target/release/te_main_fail > %t.frust.out
// RUN: diff %t.fnative.out %t.frust.out
// RUN: not cargo test --offline --release --manifest-path %t.fcrate/Cargo.toml \
// RUN:   2>&1 | FileCheck %s --check-prefix=FAILS
// FAILS: test emitrust_tests::main ... FAILED

int printf(const char *, ...);

static int state[5] = {3, 1, 4, 1, 5};

static int churn(int rounds) {
  int acc = 0;
  for (int r = 0; r < rounds; r++) {
    for (int i = 0; i < 5; i++) {
      state[i] = state[i] * 3 - (i + r);
      acc += state[i] % 7;
    }
  }
  return acc;
}

int main(void) {
  printf("acc=%d\n", churn(3));
  for (int i = 0; i < 5; i++)
    printf("state[%d]=%d\n", i, state[i]);
  return 0;
}

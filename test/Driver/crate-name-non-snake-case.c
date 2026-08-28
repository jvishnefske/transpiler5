// FR-139: the emitted manifest must not veto its own crate name.
//
// `[lints.rust] non_snake_case = "deny"` (FR-53) is a tripwire on the
// EMITTER's codegen -- it turns a regression in the idiomatic rename into a
// failed `cargo build` instead of a warning nobody reads. But rustc applies
// that lint to the CRATE NAME as well, and the name is the user's, not the
// emitter's: `--crate-name=Sieve` produced
//   error: crate `Sieve` should have a snake case name
//   = note: requested on the command line with `-D non-snake-case`
// so the crate could not be built at all. A CamelCase library name is exactly
// what a dlopen harness that looks for `libSieve.so` requires (measured on the
// TRACTOR corpus in the FR-138 spike: two otherwise-working cases lost to it).
//
// The fix relaxes the tripwire EXACTLY where it has to and nowhere else: the
// `non_snake_case` deny line is omitted when the crate name would itself trip
// it, and kept in every other case. The other two naming lints never see the
// crate name, so they are never dropped.
//
// An explicitly requested `--crate-name` is also taken at its own spelling
// now. It used to be pushed through the file-stem sanitizer, which lowercases
// -- so `Sieve` silently became `sieve` and no `lib<NAME>.so` a harness asked
// for by name could be produced. The sanitizer still governs every DERIVED
// name (an input stem, an output directory stem); only a name the user typed,
// and only when it is already a valid identifier, passes through verbatim.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --crate-name=Sieve %s \
// RUN:   -o %t.camel
// RUN: cat %t.camel/Cargo.toml | FileCheck %s --check-prefix=CAMEL
//
// The other direction, in the same test: a snake_case name KEEPS the deny.
// RUN: emitrust-cc --emit=crate --crate-type=lib --crate-name=sieve %s \
// RUN:   -o %t.snake
// RUN: cat %t.snake/Cargo.toml | FileCheck %s --check-prefix=SNAKE
//
// And a name the user did NOT type is unaffected: the derived stem is
// sanitized as it always was, and keeps the deny.
// RUN: emitrust-cc --emit=crate --crate-type=lib %s -o %t.derived
// RUN: cat %t.derived/Cargo.toml | FileCheck %s --check-prefix=DERIVED
//
// A name that is not a valid identifier still goes through the sanitizer, so
// nothing the manifest cannot hold ever reaches it.
// RUN: emitrust-cc --emit=crate --crate-type=lib --crate-name=my-lib+2 %s \
// RUN:   -o %t.odd
// RUN: cat %t.odd/Cargo.toml | FileCheck %s --check-prefix=ODD

int sieve(int n) {
  int count = 0;
  for (int i = 2; i < n; ++i) {
    int prime = 1;
    for (int d = 2; d * d <= i; ++d)
      if (i % d == 0)
        prime = 0;
    count += prime;
  }
  return count;
}

// The CamelCase name survives into BOTH tables -- `[lib] name` is what decides
// the built library's file name -- and the lint that would reject it is gone,
// while its two siblings stay.
// CAMEL:      [package]
// CAMEL-NEXT: name = "Sieve"
// CAMEL:      [lib]
// CAMEL-NEXT: name = "Sieve"
// CAMEL:      [lints.rust]
// CAMEL-NOT:  non_snake_case
// CAMEL:      non_upper_case_globals = "deny"
// CAMEL-NEXT: non_camel_case_types = "deny"

// SNAKE:      name = "sieve"
// SNAKE:      [lints.rust]
// SNAKE:      non_snake_case = "deny"
// SNAKE-NEXT: non_upper_case_globals = "deny"
// SNAKE-NEXT: non_camel_case_types = "deny"

// DERIVED:      name = "crate_name_non_snake_case"
// DERIVED:      non_snake_case = "deny"

// ODD:      name = "my_lib_2"
// ODD:      non_snake_case = "deny"

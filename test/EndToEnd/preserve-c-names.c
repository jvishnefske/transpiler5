// REQUIRES: cargo
// FR-53 opt-out: `--preserve-c-names` keeps every C spelling verbatim --
// camelCase functions, lowercase globals, snake_case struct tags -- and the
// crate must still BUILD, which requires the naming lints to move from the
// Cargo.toml deny table into the allow header (verbatim spellings legitimately
// trip them). The same input compiles under the default idiomatic rename too;
// both crates' stdout byte-matches the native binary. The greps pin the
// verbatim spellings, the mode-dependent headers, and the deny-table split.
// Since the FR-62 stage-B flip the main-only global totalCount lifts to a
// NAMED MAIN LOCAL (the E1 rule), so the spelling pins moved from the
// thread_local static to the local: verbatim mode must keep `totalCount`
// verbatim there too (the flip exposed a snake_casing of verbatim names in
// the lift's driver-local rule, fixed with this pin).
// RUN: emitrust-cc --preserve-c-names --emit=crate %s -o %t.crate --build
// RUN: grep "fn addNumbers" %t.crate/src/main.rs
// RUN: grep "let totalCount" %t.crate/src/main.rs
// RUN: grep "struct my_pair" %t.crate/src/main.rs
// RUN: grep "non_snake_case" %t.crate/src/main.rs
// RUN: not grep "non_snake_case" %t.crate/Cargo.toml
// RUN: grep 'unused_variables = "deny"' %t.crate/Cargo.toml
// RUN: emitrust-cc --emit=crate %s -o %t.renamed.crate --build
// RUN: grep "fn add_numbers" %t.renamed.crate/src/main.rs
// RUN: grep "let total_count" %t.renamed.crate/src/main.rs
// RUN: grep "struct MyPair" %t.renamed.crate/src/main.rs
// RUN: grep 'non_snake_case = "deny"' %t.renamed.crate/Cargo.toml
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/preserve_c_names > %t.preserve.out
// RUN: %t.renamed.crate/target/release/preserve_c_names > %t.renamed.out
// RUN: diff %t.native.out %t.preserve.out
// RUN: diff %t.native.out %t.renamed.out

int printf(const char *, ...);

struct my_pair {
  int first;
  int second;
};

int totalCount = 3;

int addNumbers(int a, int b) { return a + b; }

int main(void) {
  struct my_pair p;
  p.first = addNumbers(20, 2);
  p.second = totalCount;
  printf("%d %d\n", p.first, p.second);
  return 0;
}

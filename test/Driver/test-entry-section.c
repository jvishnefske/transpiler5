// FR-160 Phase C: `--test-entry-section=<name>` derives the `--test-entry`
// list from the C project's OWN registration table, so a table-registered
// test suite becomes `cargo test`-runnable with no build system, no meson
// registry and no hand-written symbol list in the loop.
//
// Why a second, purely analytical parse rather than a read of the converted
// module: the registration objects do not survive import in any stable
// shape. Measured on this very fixture -- with a `main` in the unit they
// sink into `c_main` as `emitrust.variable` LOCALS, and with no `main` the
// FR-62 actor lift eats them into `Tu0EAlphaActor` struct/impl pairs -- and
// the section string, the only stable key, is destroyed on both paths. So
// this scan takes the posture the FR-40 item graph already takes: parse the
// same inputs again, purely analytically. That also makes the scan immune to
// import failure, which this fixture exercises deliberately: `union EntryFn`
// is REJECTED by the importer (a union with a pointer arm), and its entry's
// registered function is still derived and still wrapped.
//
// This file pins:
//
// 1. THE SHAPES THAT REGISTER. A plain `{ fn, "name" }` entry, systemd's own
//    `(union f) &(fn)` cast entry (`src/shared/tests.h`'s REGISTER_TEST),
//    and an ARRAY of entries -- one object yielding two symbols -- all
//    register. The union shape is not decoration: clang traverses an
//    `InitListExpr` twice (syntactic and semantic form), so on that shape
//    every function is collected twice, and only a deduplicating collector
//    survives it. A plain-`InitListExpr`-only fixture would not reproduce
//    the defect.
//
// 2. THE EMITTED SYMBOL, NOT THE C SPELLING. An internal-linkage `t_alpha`
//    is wrapped as `tu0_t_alpha`, because the scan names functions through
//    the shared `CSymbolNaming.h` the importer itself uses. Re-deriving the
//    name from the spelling silently desyncs under FR-53 (a C `CamelCase`
//    emits as `camel_case`).
//
// 3. THE SECTION IS A FILTER. An entry registered in a DIFFERENT section is
//    not a test.
//
// 4. DEDUP, ONCE, FOR EVERY SOURCE. The same function registered twice AND
//    also named by hand on `--test-entry` is wrapped exactly once. Two
//    identically named `fn` items in one module is an unbuildable crate
//    (rustc E0428), so this is correctness, not tidiness. The Phase A
//    sources have the same hazard on their own -- a repeated `--test-entry`
//    emitted two `fn`s before this wave -- so the pure Phase A shape and the
//    `--test-entries` file are pinned here too; the dedup is one gate for
//    all three sources.
//
// 5. THE SCAN IS SOURCE-SIDE, so it is REJECTED where there are no sources
//    to scan. Under `--link` the inputs are shards clang cannot parse, and
//    the merge alpha-renames each shard's `tu0_` tag to its link-line
//    position, so even a source-side scan would derive wrong symbols; under
//    `--emit=mlir` and friends there is no crate root to append to. Both are
//    hard errors rather than a silently ignored flag.
//
// 6. A SECTION NOBODY CARRIES IS A LOCATED WARNING, never a silent success:
//    the caller asked for a table and got nothing, and zero tests that pass
//    is precisely the vacuous pass FR-160 exists to forbid.
//
// 7. WITHOUT THE FLAG NOT ONE BYTE MOVES -- the same guard `test-entry.c`
//    holds for `--test-entry`, checked explicitly rather than assumed,
//    because the flag has to be in the byte-identity early-return or it is
//    a silent no-op.
//
// Everything past the derivation is Phase A's policy unchanged: the entries
// go through the same list, the same wrap/skip/ignore arms and the same
// located diagnostics.
//
// RUN: emitrust-cc --emit=rust --crate-type=lib --recover %s \
// RUN:   --test-entry-section=MY_TEST_TABLE --test-entry=tu0_t_dup \
// RUN:   -o %t.rs 2>%t.err
// RUN: FileCheck %s --check-prefix=TESTS --input-file=%t.rs
//
// The other table's registration is not in this table. (The crate defines
// the function itself, so the negative is on the generated CALL, which only
// a wrapper emits.)
// RUN: not grep 'super::tu0_t_other' %t.rs
// Registered twice and hand-named too, wrapped once.
// RUN: grep -c 'super::tu0_t_dup()' %t.rs | FileCheck %s --check-prefix=ONCE
// The union-cast entry is collected twice by clang's double InitListExpr
// traversal and still wrapped once.
// RUN: grep -c 'super::tu0_t_union()' %t.rs | FileCheck %s --check-prefix=ONCE
// ONCE: 1
//
// The same dedup covers the (until now unpinned) `--test-entries` file.
// RUN: echo 'tu0_t_alpha # from the project test registry' > %t.entries
// RUN: emitrust-cc --emit=rust --crate-type=lib --recover %s \
// RUN:   --test-entry-section=MY_TEST_TABLE --test-entries=%t.entries \
// RUN:   -o %t.file.rs 2>%t.file.err
// RUN: grep -c 'super::tu0_t_alpha()' %t.file.rs | FileCheck %s --check-prefix=ONCE
//
// ... and the Phase A source on its own: a repeated --test-entry used to
// emit two identically named `fn`s, an unbuildable crate.
// RUN: emitrust-cc --emit=rust --crate-type=lib --recover %s \
// RUN:   --test-entry=tu0_t_dup --test-entry=tu0_t_dup -o %t.twice.rs 2>&1
// RUN: grep -c 'super::tu0_t_dup()' %t.twice.rs | FileCheck %s --check-prefix=ONCE
//
// Both `appendTestModule` call sites see the derived list, so --emit=crate
// carries the tests as well as --emit=rust.
// RUN: emitrust-cc --emit=crate --crate-type=lib --recover %s \
// RUN:   --test-entry-section=MY_TEST_TABLE -o %t.crate 2>&1
// RUN: grep -c 'super::tu0_t_alpha()' %t.crate/src/lib.rs \
// RUN:   | FileCheck %s --check-prefix=ONCE
//
// A section no object carries emits no test and says so, with a location.
// RUN: emitrust-cc --emit=rust --crate-type=lib --recover %s \
// RUN:   --test-entry-section=NO_SUCH_TABLE -o %t.none.rs 2>%t.none.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.none.err
// RUN: not grep 'cfg(test)' %t.none.rs
//
// There are no sources to scan under --link, and no crate root to append to
// under --emit=mlir: a flag that cannot do its job is a located error, not a
// silent no-op.
// RUN: not emitrust-cc --emit=rust --link --test-entry-section=MY_TEST_TABLE \
// RUN:   %s -o %t.link.rs 2>&1 | FileCheck %s --check-prefix=NOLINK
// RUN: not emitrust-cc --emit=mlir --test-entry-section=MY_TEST_TABLE %s \
// RUN:   -o %t.mlir 2>&1 | FileCheck %s --check-prefix=NOLINK
// NOLINK: error: --test-entry-section is only valid with --emit=crate or --emit=rust, and not with --link: it scans the C sources, which a link line does not name
//
// The byte-identity guard: the same input with no section requested is
// exactly today's output, and silent about FR-160.
// RUN: emitrust-cc --emit=rust --crate-type=lib --recover %s -o %t.plain.rs \
// RUN:   2>%t.plain.err
// RUN: not grep 'cfg(test)' %t.plain.rs
// RUN: not grep FR-160 %t.plain.err
// The flagged output's first N lines ARE the unflagged output, byte for
// byte; everything the flag adds comes after them.
// RUN: head -n `wc -l < %t.plain.rs` %t.rs | diff %t.plain.rs -

typedef struct TestFunc {
  void (*f)(void);
  const char *name;
} TestFunc;

// systemd's own entry shape: the address is stored through a cast to a union
// of function-pointer arms, so the initializer is a `CStyleCastExpr <ToUnion>`
// over `UnaryOperator '&'` over the `DeclRefExpr` -- not the bare
// `FunctionToPointerDecay` the plain shape produces.
union EntryFn {
  void (*void_func)(void);
  int (*int_func)(void);
};

typedef struct UnionFunc {
  union EntryFn f;
  const char *name;
} UnionFunc;

#define TABLE __attribute__((section("MY_TEST_TABLE"), used, aligned(16)))
#define OTHER __attribute__((section("OTHER_TABLE"), used, aligned(16)))

static void t_alpha(void) {}
static void t_beta(void) {}
static void t_union(void) {}
static void t_arr_one(void) {}
static void t_arr_two(void) {}
static void t_dup(void) {}
static void t_other(void) {}

TABLE static const TestFunc e_alpha = {t_alpha, "t_alpha"};
TABLE static const TestFunc e_beta = {t_beta, "t_beta"};
TABLE static const UnionFunc e_union = {(union EntryFn) & (t_union), "t_union"};
TABLE static const TestFunc e_arr[2] = {{t_arr_one, "one"}, {t_arr_two, "two"}};
TABLE static const TestFunc e_dup1 = {t_dup, "dup"};
TABLE static const TestFunc e_dup2 = {t_dup, "dup again"};
OTHER static const TestFunc e_other = {t_other, "t_other"};

// The hand-named entry comes first (`--test-entry` is applied before the
// derived list), then the table in registration order.
// TESTS:      #[cfg(test)]
// TESTS-NEXT: #[allow(non_snake_case)]
// TESTS-NEXT: mod emitrust_tests {
//
// TESTS:      #[test]
// TESTS-NEXT: fn tu0_t_dup() {
// TESTS-NEXT: super::tu0_t_dup();
//
// TESTS:      #[test]
// TESTS-NEXT: fn tu0_t_alpha() {
// TESTS-NEXT: super::tu0_t_alpha();
//
// TESTS:      #[test]
// TESTS-NEXT: fn tu0_t_beta() {
// TESTS-NEXT: super::tu0_t_beta();
//
// TESTS:      #[test]
// TESTS-NEXT: fn tu0_t_union() {
// TESTS-NEXT: super::tu0_t_union();
//
// TESTS:      #[test]
// TESTS-NEXT: fn tu0_t_arr_one() {
// TESTS-NEXT: super::tu0_t_arr_one();
//
// TESTS:      #[test]
// TESTS-NEXT: fn tu0_t_arr_two() {
// TESTS-NEXT: super::tu0_t_arr_two();
//
// WARN: warning: FR-160: --test-entry-section='NO_SUCH_TABLE' matched no object; no test entry point was registered in that section

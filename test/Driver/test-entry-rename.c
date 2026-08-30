// FR-160b: a requested test entry point is spelled as the crate EMITS it, and
// a request that does not match one is never silently dropped.
//
// FR-160's governing rule is NEVER A VACUOUS PASS, and its corollary is never
// a silent request-ignore: a request that cannot be honoured must say so, with
// a location. Landed FR-160 broke the corollary in three ways, all MEASURED by
// the --partition spike, and this file pins the fix for each:
//
// 1. THE RENAME. FR-53 renames a C `camelCaseTest` to `camel_case_test` and
//    tags an internal-linkage `t_helper` as `tu0_t_helper`, and nothing said
//    so: `--test-entry=camelCaseTest` reported "no function of that name in
//    this crate" while the crate plainly defined it under the renamed
//    spelling. The reverse mapping is NOT applied automatically, and that is
//    deliberate, not laziness -- it was measured UNSOUND. Two translation
//    units, one defining `foo_bar` and one defining `fooBar`, both emit
//    `foo_bar`; a whole-project registry naming `fooBar` applied per TU (which
//    is FR-160's actual mode) would then wrap the WRONG function, and a wrong
//    wrap in a differential oracle is a false RED or a false GREEN -- strictly
//    worse than the skip it replaces. So the crate's own definition is offered
//    as a HINT, located at that definition, and nothing is wrapped.
//
// 2. THE `--test-entries` FILE'S SILENCE. A whole-project registry names every
//    unit's tests, so a symbol this unit does not define is skipped silently --
//    correct, and it must stay. But a resolving hint is PROOF the symbol is in
//    this crate under another spelling, which the "another unit owns it"
//    rationale does not cover. The suppression is lifted exactly there: a
//    file-sourced entry with a hint warns, a file-sourced entry with no
//    candidate anywhere stays silent.
//
// 3. `main`. This is the one alias that DOES wrap, and it is the motivating
//    path: scripts/test-entries-meson.py's default mode writes the literal
//    symbol `main` for every one-TU test (337 of them on systemd), and the
//    emitter renames `main` to `c_main` unconditionally -- `--preserve-c-names`
//    does not turn it off. Consumed through `--test-entries` that combination
//    yielded ZERO tests and ZERO words: the FR's own Phase B output failing
//    against its own Phase A. The alias is injective by a hard error rather
//    than by convention (a unit defining both `main` and `c_main` is rejected
//    "function name 'c_main' is reserved for the imported C main"), which is
//    what makes it safe to wrap where the casing alias is not.
//
// 4. THE ACTOR ARM AND THE EXTERN each get a reason that is TRUE. Both used to
//    report "no function of that name in this crate", and both had a dedicated
//    arm that could never run: the FR-62 actor lift nests the arm inside
//    `emitrust.impl`, which is its OWN SymbolTable, so a module-level lookup
//    structurally cannot see it (and the conversion strips the
//    `emitrust.method_of` attribute the dead arm tested for); a referenced
//    extern becomes an `Externals` TRAIT MEMBER and never a module-level func
//    at all. The actor arm is reported, not wrapped, because the struct's C
//    initializers are applied only inside `c_main` -- a `Default`-constructed
//    receiver was measured to turn a passing program into a failing test.
//
// A hint is NOT a wrap: none of the reported entries below produce a
// `#[cfg(test)]` block at all.
//
// RUN: emitrust-cc --emit=rust --crate-type=lib %s \
// RUN:   --test-entry=camelCaseTest --test-entry=t_helper \
// RUN:   --test-entry=extern_test --test-entry=nowhere_at_all \
// RUN:   -o %t.rs 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: not grep 'cfg(test)' %t.rs
//
// WARN: warning: FR-160: no test emitted for 'camelCaseTest': no function of that name in this crate; did you mean 'camel_case_test'? A test entry is spelled as EMITTED, and the rename is not reversed automatically because two C spellings can fold onto one Rust name
// WARN: warning: FR-160: no test emitted for 't_helper': no function of that name in this crate; did you mean 'tu0_t_helper'?
// WARN: warning: FR-160: no test emitted for 'extern_test': declared in this unit but defined in another; link the shards to wrap it
// WARN: warning: FR-160: no test emitted for 'nowhere_at_all': no function of that name in this crate
//
// The hint is located at the definition it names, not at the crate root -- the
// whole point is to put the caller in front of the spelling to use.
// RUN: FileCheck %s --check-prefix=WHERE --input-file=%t.err
//
// (2) the `--test-entries` file's silent-skip rule is lifted exactly where a
// candidate resolves, and kept everywhere else.
// RUN: echo 'camelCaseTest' > %t.entries
// RUN: echo 'nowhere_at_all' >> %t.entries
// RUN: emitrust-cc --emit=rust --crate-type=lib %s --test-entries=%t.entries \
// RUN:   -o %t.file.rs 2>%t.file.err
// RUN: grep "no test emitted for 'camelCaseTest'" %t.file.err
// RUN: not grep 'nowhere_at_all' %t.file.err
//
// (3) `main` wraps, through the file source the adapter actually writes.
// RUN: echo 'main' > %t.main.entries
// RUN: emitrust-cc --emit=rust %S/Inputs/test-entry-rename/main-entry.c \
// RUN:   --test-entries=%t.main.entries -o %t.main.rs 2>%t.main.err
// RUN: not grep FR-160 %t.main.err
// RUN: FileCheck %s --check-prefix=MAIN --input-file=%t.main.rs
// MAIN:      #[test]
// MAIN-NEXT: fn main() {
// MAIN-NEXT: assert_eq!(super::c_main(), 0);
//
// ... and the emitted spelling keeps working, and asking for BOTH is not an
// unbuildable crate: the dedup is on the REQUESTED string, so the two wrappers
// get distinct names.
// RUN: emitrust-cc --emit=rust %S/Inputs/test-entry-rename/main-entry.c \
// RUN:   --test-entry=main --test-entry=c_main -o %t.both.rs 2>%t.both.err
// RUN: not grep FR-160 %t.both.err
// RUN: grep -c 'assert_eq!(super::c_main(), 0);' %t.both.rs \
// RUN:   | FileCheck %s --check-prefix=TWICE
// TWICE: 2
//
// (4) the actor arm's reason is the actor arm's reason, located at the arm.
// RUN: emitrust-cc --emit=rust %S/Inputs/test-entry-rename/actor-entry.c \
// RUN:   --test-entry=test_alpha -o %t.actor.rs 2>%t.actor.err
// RUN: FileCheck %s --check-prefix=ACTOR --input-file=%t.actor.err
// RUN: not grep 'cfg(test)' %t.actor.rs
// ACTOR: actor-entry.c:{{[0-9]+}}:{{[0-9]+}}: warning: FR-160: no test emitted for 'test_alpha': rendered as a method of impl 'Tu0HelperCountActor', not callable as a free function; the FR-62 actor lift moved it there because it touches a file-local global, and a Default-constructed receiver would not carry that global's C initializer
//
// (5) WITHOUT the flag not one byte moves, and with a request that wraps
// NOTHING the crate is still byte-identical -- a hint is a diagnostic, never
// emitted code.
// RUN: emitrust-cc --emit=rust --crate-type=lib %s -o %t.plain.rs 2>%t.plain.err
// RUN: not grep FR-160 %t.plain.err
// RUN: not grep 'cfg(test)' %t.plain.rs
// RUN: diff %t.plain.rs %t.rs

// WHERE: test-entry-rename.c:[[@LINE+1]]:{{[0-9]+}}: warning: FR-160: no test emitted for 'camelCaseTest'
int camelCaseTest(void) { return 0; }

static int t_helper(void) { return 3; }
int uses_helper(void) { return t_helper() - 3; }

int extern_test(void);
int caller(void) { return extern_test(); }

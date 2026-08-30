// FR-160 under FR-59 `--partition`: the generated `#[cfg(test)]` module in a
// WORKSPACE. Before this wave every `--test-entry` and `--test-entries`
// request under `--partition` was accepted, exited 0, and did nothing at all
// -- not one diagnostic, not one `#[test]`. A silently ignored request is the
// exact failure mode this FR forbids, so this file pins the whole
// per-member policy.
//
// The design problem, and why it is not a one-line call: an entry-point
// symbol lives in exactly ONE member, so the per-member decision is local
// while the DIAGNOSTIC is a whole-workspace judgement. Applying the entry
// list to N members and reporting per member would emit N-1 "no function of
// that name in this crate" warnings for a symbol that is perfectly fine.
//
// This file pins:
//
// 1. PLACEMENT IS BY DEFINITION, not by hope. The `#[cfg(test)] mod` lands in
//    the member that DEFINES the entry and in no other -- the integer entry
//    in `liba`, the void entry in `libb`, neither in the binary member. This
//    is correctness, not tidiness: the binary member's root carries
//    `use liba::*; use libb::*;`, so a test wrapping `super::test_alpha()`
//    there COMPILES AND PASSES in a crate that defines neither symbol
//    (measured). The only thing standing between this FR and a duplicated,
//    misattributed green test is the symbol-table lookup against the
//    member's OWN module.
//
// 2. EXACTLY ONE LOCATED WARNING for a hand-named symbol NO member defines --
//    not one per member -- reported against the merged module, which still
//    carries a real source location. A member module is created with
//    `UnknownLoc` and would render as `<unknown>:0:`, which is not located in
//    any useful sense, so the `.c:1:1:` prefix is checked here and not just
//    the wording.
//
// 3. A SYMBOL SOME MEMBER DEFINES BUT CANNOT WRAP is reported against ITS
//    OWN definition, once, with a real file:line:col -- the source locations
//    survive the merge and the split into member modules.
//
// 4. NO WARNING AT ALL when every hand-named symbol was wrapped somewhere.
//
// 5. THE `--test-entries` FILE KEEPS ITS SILENT-SKIP RULE: a whole-project
//    registry names every unit's tests, so a symbol no member defines is not
//    a diagnostic. The file is read ONCE for the whole workspace, so an
//    unreadable one is one error, not one per member.
//
// 6. `--test-entry-section` STAYS REJECTED here. `--partition` requires
//    `--link`, and the section scan is source-side: the merge alpha-renames
//    each shard's `tu0_` tag to its link-line position, so a source-side scan
//    would derive symbols the merged module does not have. A recorded NO-GO,
//    re-pinned under an explicit `--partition` run.
//
// 7. AN EMISSION WITH NO CRATE ROOT REJECTS THE FLAG. `--partition
//    --emit=ratchet` returns from the artifact query before any root is
//    rendered, so a test entry request there was a fourth silent ignore.
//
// 8. WITHOUT A REQUEST NOT ONE BYTE MOVES, per member and per manifest.
//
// Shim-built shards, one per member directory (link-line order fixes the
// merged module's location and the `tuN_` tags):
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/test-entry-partition/liba/a.c -o %t.a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/test-entry-partition/libb/b.c -o %t.b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
//
// (1) + (4): each entry wrapped in its OWN member, and silence.
// RUN: rm -rf %t.ws
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.ws --crate-name papp \
// RUN:   --emit=crate --partition --test-entry=test_alpha \
// RUN:   --test-entry=test_beta 2>%t.err
// RUN: FileCheck %s --check-prefix=ALPHA --input-file=%t.ws/liba/src/lib.rs
// RUN: FileCheck %s --check-prefix=BETA --input-file=%t.ws/libb/src/lib.rs
// RUN: not grep FR-160 %t.err
//
// The other members do not get it. The binary member is the dangerous one:
// its glob imports would make either test compile and pass there.
// RUN: not grep 'cfg(test)' %t.ws/papp/src/main.rs
// RUN: not grep 'test_beta' %t.ws/liba/src/lib.rs
// RUN: not grep 'test_alpha' %t.ws/libb/src/lib.rs
//
// (2) exactly one located warning for a symbol nothing defines.
// RUN: rm -rf %t.ws2
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.ws2 --crate-name papp \
// RUN:   --emit=crate --partition --test-entry=absolutely_nowhere 2>%t.err2
// RUN: grep -c "absolutely_nowhere" %t.err2 | FileCheck %s --check-prefix=ONCE
// ONCE: 1
// RUN: FileCheck %s --check-prefix=NOWHERE --input-file=%t.err2
// NOWHERE: a.c:1:1: warning: FR-160: no test emitted for 'absolutely_nowhere': no function of that name in this crate
// RUN: not grep -r 'cfg(test)' %t.ws2
//
// (3) a symbol libb defines but cannot wrap: one warning, at ITS definition.
// RUN: rm -rf %t.ws3
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.ws3 --crate-name papp \
// RUN:   --emit=crate --partition --test-entry=test_takes 2>%t.err3
// RUN: grep -c "test_takes" %t.err3 | FileCheck %s --check-prefix=ONCE
// RUN: FileCheck %s --check-prefix=TAKES --input-file=%t.err3
// TAKES: b.c:16:5: warning: FR-160: no test emitted for 'test_takes': takes arguments; a test entry point must take none
// RUN: not grep -r 'cfg(test)' %t.ws3
//
// (5) the entries FILE: the defined symbol is wrapped in its member, the
// undefined one is skipped in silence.
// RUN: echo 'test_alpha' > %t.entries
// RUN: echo 'absolutely_nowhere # another unit owns this one' >> %t.entries
// RUN: rm -rf %t.ws4
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.ws4 --crate-name papp \
// RUN:   --emit=crate --partition --test-entries=%t.entries 2>%t.err4
// RUN: not grep FR-160 %t.err4
// RUN: grep 'super::test_alpha()' %t.ws4/liba/src/lib.rs
// RUN: not grep 'cfg(test)' %t.ws4/libb/src/lib.rs
//
// ... and it is read ONCE for the whole workspace, not once per member.
// RUN: rm -rf %t.ws5
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.ws5 --crate-name papp \
// RUN:   --emit=crate --partition --test-entries=%t.no.such.file 2>%t.err5
// RUN: grep -c "cannot read --test-entries file" %t.err5 \
// RUN:   | FileCheck %s --check-prefix=ONCE
//
// (6) the section scan cannot be trusted across a link line, so it is a
// located error under --partition too, not a silent no-op.
// RUN: not emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.ws6 \
// RUN:   --crate-name papp --emit=crate --partition \
// RUN:   --test-entry-section=MY_TEST_TABLE 2>&1 \
// RUN:   | FileCheck %s --check-prefix=NOSECTION
// NOSECTION: error: --test-entry-section is only valid with --emit=crate or --emit=rust, and not with --link: it scans the C sources, which a link line does not name
//
// (7) an emission that renders no crate root rejects the request instead of
// dropping it.
// RUN: not emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.ratchet \
// RUN:   --crate-name papp --emit=ratchet --partition --test-entry=test_alpha \
// RUN:   2>&1 | FileCheck %s --check-prefix=NOROOT
// RUN: not emitrust-cc --emit=mlir %S/Inputs/test-entry-partition/liba/a.c \
// RUN:   -o %t.mlir --test-entries=%t.entries 2>&1 \
// RUN:   | FileCheck %s --check-prefix=NOROOT
// NOROOT: error: --test-entry/--test-entries is only valid with --emit=crate or --emit=rust: no other emission renders a crate root for the generated tests to be appended to
//
// (8) byte identity, per member and per manifest. The requested run's crate
// roots begin with exactly the unrequested run's bytes; everything the flag
// adds comes after them, and the manifests do not move at all.
// RUN: rm -rf %t.plain
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.plain \
// RUN:   --crate-name papp --emit=crate --partition 2>%t.plain.err
// RUN: not grep FR-160 %t.plain.err
// RUN: not grep -r 'cfg(test)' %t.plain
// RUN: head -n `wc -l < %t.plain/liba/src/lib.rs` %t.ws/liba/src/lib.rs | diff %t.plain/liba/src/lib.rs -
// RUN: head -n `wc -l < %t.plain/libb/src/lib.rs` %t.ws/libb/src/lib.rs | diff %t.plain/libb/src/lib.rs -
// RUN: diff %t.plain/papp/src/main.rs %t.ws/papp/src/main.rs
// RUN: diff %t.plain/Cargo.toml %t.ws/Cargo.toml
// RUN: diff %t.plain/liba/Cargo.toml %t.ws/liba/Cargo.toml
// RUN: diff %t.plain/libb/Cargo.toml %t.ws/libb/Cargo.toml
// RUN: diff %t.plain/papp/Cargo.toml %t.ws/papp/Cargo.toml

int test_alpha(void);
void test_beta(void);

int printf(const char *, ...);

int main(void) {
  test_beta();
  printf("alpha=%d\n", test_alpha());
  return 0;
}

// The integer entry, wrapped in the member that defines it.
// ALPHA:      #[cfg(test)]
// ALPHA-NEXT: #[allow(non_snake_case)]
// ALPHA-NEXT: mod emitrust_tests {
// ALPHA:      #[test]
// ALPHA-NEXT: fn test_alpha() {
// ALPHA-NEXT: assert_eq!(super::test_alpha(), 0);
//
// The void entry, run for its panics, in ITS member.
// BETA:      #[cfg(test)]
// BETA-NEXT: #[allow(non_snake_case)]
// BETA-NEXT: mod emitrust_tests {
// BETA:      #[test]
// BETA-NEXT: fn test_beta() {
// BETA-NEXT: super::test_beta();

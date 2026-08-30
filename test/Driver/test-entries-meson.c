// FR-160 Phase B: `scripts/test-entries-meson.py` is the discovery adapter
// that turns a meson project's test registry into a `--test-entries` file.
// The seam it defends is the whole reason FR-160 is one feature and not a
// systemd feature: the EMITTER never learns what a build system is, it takes
// a list of symbols, and knowing how to produce that list from a project
// lives out here. A CTest adapter is the same shape over
// `ctest --show-only=json-v1`.
//
// The fixture is a checked-in `meson-info/` pair, not a configured project,
// and that is deliberate: the adapter is a pure data transformation, so
// testing it needs no meson, no compiler and no network, and the test cannot
// rot when a real project's build files change.
//
// This file pins the SELECTION RULES, each of which exists because the
// registry contains far more than compiled C tests -- on real systemd, 4518
// tests of which only 337 are single-TU C binaries:
//
// 1. A test whose `cmd[0]` is a built C target with exactly ONE source is
//    selected; that is the clean TU <-> test-binary mapping the per-unit
//    crate emission already produces.
// 2. `protocol` must be `exitcode`. That is the criterion `#[test]` inherits
//    (panic = fail); a `tap` test reports through its stdout, which nothing
//    here models, so wrapping it would assert something it does not mean.
// 3. A script test -- `cmd[0]` is not a built target at all -- is skipped.
// 4. A MULTI-SOURCE target is skipped: its symbols only exist after `--link`,
//    and a per-TU entry list would name functions the crate does not have.
// 5. Two meson tests invoking the SAME binary collapse to ONE entry, with the
//    count kept. argv is not modelled, so a parameterized test's variants are
//    indistinguishable here; emitting the line twice would tell a reader
//    there are two tests to run when there is one thing this can express.
//
// Every skip is COUNTED and printed to stderr. An adapter that silently
// dropped four fifths of a registry would be the same silent request-ignore
// FR-176 had to clean up on the emitter side.
//
// RUN: %python %S/../../scripts/test-entries-meson.py \
// RUN:   --build %S/Inputs/test-entries-meson > %t.entries 2> %t.stats
// RUN: FileCheck %s --check-prefix=ENTRIES --input-file=%t.entries
// RUN: FileCheck %s --check-prefix=STATS --input-file=%t.stats
//
// Exactly one entry line survives all five rules.
// RUN: grep -vc '^#' %t.entries > %t.count
// RUN: FileCheck %s --check-prefix=COUNT --input-file=%t.count
//
// The output is the format `--test-entries` reads: symbol first, provenance
// in a trailing comment. Round-trip it through the reader to prove the two
// halves agree -- the adapter's whole job is to feed that flag.
// RUN: emitrust-cc --emit=rust --crate-type=lib %S/Inputs/test-entries-meson/entry.c \
// RUN:   --test-entries=%t.entries -o %t.rs 2>%t.err
// RUN: FileCheck %s --check-prefix=ROUNDTRIP --input-file=%t.rs
// RUN: not grep FR-160 %t.err

// ENTRIES: {{^}}main{{[[:space:]]}}# meson test 'test-alpha' (test-alpha.c) +1 more
// ENTRIES-NOT: test-multi
// ENTRIES-NOT: script-test
// ENTRIES-NOT: tap-test

// COUNT: 1

// STATS-DAG: skip: cmd[0] is not a built target           1
// STATS-DAG: skip: protocol != exitcode                   1
// STATS-DAG: skip: multi-source target (needs --link)     1
// STATS-DAG: dedup: same symbol, another meson test       1
// STATS-DAG: entries                                      1

// The reader wraps `main` through the FR-176 alias: the emitted entry point
// of a C `main` is `c_main`, and that mapping is sound because `c_main` is a
// reserved name.
// ROUNDTRIP:      #[cfg(test)]
// ROUNDTRIP:      fn main() {
// ROUNDTRIP-NEXT: assert_eq!(super::c_main(), 0);

// RUN: %python %S/../../scripts/check-rejection-ledger.py --repo %S/../..
//
// FR-241: pins the LIVENESS and MIRROR-EQUALITY of the blocker-tag tables in
// lib/ImportC/RejectionLedger.cpp against their hand-written Python twin,
// classify_blocker in test/RealWorld/run_realworld.py.
//
// WHY THIS TEST EXISTS. The tag vocabulary is how a reader ranks work: it is
// read by tools/emitrust-cc/RatchetReport.cpp's rejection report, by
// FrontierSearch.cpp, by ShardMetadata.h, by the ledger's own blocker
// tabulation, and by the RealWorld survey. It is produced by two hand-written
// substring tables keyed on DIAGNOSTIC WORDING, with nothing compiled tying a
// needle to the emitError that raises it -- and FR-241's mechanical audit
// measured the consequence: 54 of 89 tags are asserted NOWHERE in test/ except
// inside the Python mirror itself, and SEVEN defects had accumulated unseen.
// Two needles were DEAD (`written with a null pointer`, `explicit class
// template specialization` -- rejections that had been admitted or reworded,
// leaving a row that tells a census reader a frontier still exists when
// nothing can raise it); two rows existed in C++ and not in Python (`has no
// bit-exact Rust mapping`, dark since FR-224 minted it precisely to keep
// `expf` out of `other`; `was not reached: sibling specialization`); two
// node-named families had no row at all; one row had been UNREACHABLE since
// it was minted, and one broad needle was stealing another row's traffic.
//
// WHAT IT ASSERTS (details and the soundness argument are in the script's
// module docstring): (a) every needle is a substring of a real diagnostic
// literal in lib/ImportC, outside the ledger itself, with an audited
// `LIVE_VIA` waiver for the one wording built around an interpolation;
// (b) the three tables are row-for-row identical across the two mirrors;
// (c) no row is unreachable, either by needle nesting or because an earlier
// row claims every wording it occurs in.
//
// WHAT IT DELIBERATELY DOES NOT ASSERT: anything about the `other` bucket.
// 607 of 802 assembled messages classify `other` and most legitimately --
// they are internal consistency checks with zero corpus hits -- so an
// allowlist would need a line per new diagnostic and review pressure would
// push people to add the line rather than think about the tag. The `other`
// wordings that appear in a real case's blocker set are PRINTED instead, as
// is the needle-ambiguity inventory; both are reports, never gates.
//
// It runs in the fast tier so the pre-commit gate always sees it. This file
// is a .c only so lit discovers it; it contains no C. Same shape as
// test/Driver/backlog-check.c, which pins design.md against backlog.toml.

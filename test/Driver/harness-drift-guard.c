// RUN: %python %S/../../nix/harness/test_harness.py
//
// Pins the improvement harness's frozen-population invariants (FR-144) in the
// fast lit tier. The harness lives under nix/ and its clippy ratchet is far
// too slow to gate (it transpiles and clippy-lints 238 crates), which is
// exactly why two silent violations of its paired-comparison premise survived
// for weeks: epochs 1-3 drifted while `ledger_append` kept recording against
// them, and the committed ratchet compared a 170-crate measurement against a
// 164-crate baseline. The guards are pure Python and hermetic (synthetic
// corpora in tempdirs), so THEY can be gated even though the measurement
// cannot: a drifted or closed epoch is refused loudly, a closure never
// rewrites the epoch document, and the three consumers of "which baseline is
// authoritative" must name the same pinned file. See nix/harness/
// test_harness.py for the per-case intent. This file is a .c only so lit
// discovers it; it contains no C.

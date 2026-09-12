// REQUIRES: cargo
// RUN: %python %S/run_cppstd_suite.py --emitrust-cc emitrust-cc --suite %S/../../third_party/llvm-test-suite --candidates %S/candidates.txt --expected-dir %S/expected --manifest %S/expected-pass.txt --known-miscompiles %S/known-miscompiles.txt --workdir %t
//
// Standard-C++ conformance ledger over the llvm-test-suite submodule
// (FR-244).  This file has a .cpp suffix only so lit discovers it
// (test/lit.cfg.py sets config.suffixes = [".mlir", ".c", ".cpp"]); it
// contains no C++ code.  The real work is run_cppstd_suite.py (a thin
// wrapper over test/harness/ledger_runner.py), which runs every
// candidates.txt entry through emitrust-cc --emit=crate --build and
// enforces the expected-pass.txt ledger.  The corpus is CURATED, never
// globbed: candidates.txt lists the third_party/llvm-test-suite programs
// that fit the ledger contract (bare main, no stdin, exit 0,
// deterministic byte-exact stdout), selected reproducibly by
// curate_candidates.py.
//
// A missing submodule is a HARD FAIL with an init hint, the same
// doctrine as CTestSuite: a lit UNSUPPORTED-skip would silently drop the
// whole ledger while the gate stayed green, which is exactly the hole
// the two-way ratchet exists to close.  Worktrees therefore need
//   git submodule update --init third_party/c-testsuite third_party/llvm-test-suite
// (add --reference /home/j/transpiler5 to dedupe the checkout cost).
//
// Tool resolution: lit's ToolSubst (registered for "emitrust-cc" in
// test/lit.cfg.py) rewrites the bare token `emitrust-cc` after
// --emitrust-cc above to the absolute path of the built tool; the flag
// name `--emitrust-cc` itself is NOT rewritten because ToolSubst skips
// matches whose preceding character is '-'.  As a fallback for manual
// invocation, the script also resolves a bare name via shutil.which.
//
// Outcome model: most corpus entries are UNSUPPORTED (beyond the
// supported C++17 subset -- the UNSUPPORTED count IS the frontier
// measurement); tests in expected-pass.txt must PASS; any successfully
// transpiled test with wrong runtime behavior is a MISCOMPILE and fails
// the run unless explicitly quarantined in known-miscompiles.txt, and a
// new quarantine entry requires a defect FR in the same commit.
// Day-one ledger (2026-09-11, submodule pin 4b2c892865cb): 65 candidates,
// 11 PASS, 0 MISCOMPILE, 54 UNSUPPORTED.
//
// After extending the transpiler, ratchet the ledger with:
//   python3 test/CppStdSuite/run_cppstd_suite.py --emitrust-cc build/tools/emitrust-cc \
//     --suite third_party/llvm-test-suite --candidates test/CppStdSuite/candidates.txt \
//     --expected-dir test/CppStdSuite/expected --manifest test/CppStdSuite/expected-pass.txt \
//     --workdir /tmp/cppstd-work --update
//
// PIN-ADVANCE PROCEDURE (moving the submodule SHA), all in ONE commit:
//   1. bump third_party/llvm-test-suite to the new SHA;
//   2. re-run the curation, which rewrites candidates.txt AND expected/:
//        nix develop -c python3 test/CppStdSuite/curate_candidates.py \
//          --emit-expected test/CppStdSuite/expected
//      (delete test/CppStdSuite/expected/ first so removed programs do
//      not leave stale .expected files behind);
//   3. re-run the ledger with --update as above;
//   4. commit the pin, candidates.txt, expected/, and expected-pass.txt
//      together, recording the counts in the FR that motivated the bump.
